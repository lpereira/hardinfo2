/*
 * livegraph.c - A live-updating graph widget for GTK 2, 3 and 4.
 *
 * Compiles against GTK2, GTK3 or GTK4 from the same source using the
 * version macros in the installed headers. Provides a custom widget that
 * keeps a ring buffer of samples and redraws itself as data is pushed.
 *
 * Written by: hwspeedy
 * License: GPL2+
 *
 */

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include "livegraph.h"

/* Detect the GTK major version we compile against. */
#if !defined(GTK_MAJOR_VERSION)
#  include <gtk/gtkversion.h>
#endif

#if GTK_MAJOR_VERSION == 4
#  define LG_GTK4
#elif GTK_MAJOR_VERSION == 3
#  define LG_GTK3
#else
#  define LG_GTK2
#endif

#define LG_N_SIGNALS   10

/* Plain RGBA (no GdkRGBA: that type does not exist in GTK2). */
typedef struct {
    double r, g, b, a;
} LgRGBA;

typedef struct _LiveGraphClass LiveGraphClass;

struct _LiveGraph {
    GtkWidget parent;

    /* Ring buffer of normalized samples in [0,1], one per signal.
     * Flat array of size n_signals * max_samples, indexed as
     * [signal * max_samples + pos]. Allocated on the heap to keep the
     * GObject instance size small (GTK4 requires instance_size <= 65535). */
    gdouble *samples;
    gint    max_samples;
    gint    write_pos;
    gint    count;

    /* Monotonic timestamp (seconds) at which each sample frame was pushed,
     * plus the base time used to compute the elapsed seconds. */
    gdouble *sample_time;
    gdouble  t0;

    /* How many of the most recent samples to display. */
    gint    window;

    LiveGraphTheme theme;       /* requested theme (auto/light/dark) */

    /* Color sets chosen at render time based on the resolved theme. */
    LgRGBA bg_color;
    LgRGBA grid_color;
    LgRGBA border_color;
    LgRGBA label_color;

    LgRGBA line_colors_dark[LG_N_SIGNALS];
    LgRGBA line_colors_light[LG_N_SIGNALS];
    LgRGBA *line_colors;        /* -> one of the two palettes, active */

    gint    selected_line;      /* signal index drawn thicker, -1 = none */
};

struct _LiveGraphClass {
    GtkWidgetClass parent_class;
};

#define LIVE_TYPE_GRAPH    (live_graph_get_type ())
#define LIVE_GRAPH(obj)    (G_TYPE_CHECK_INSTANCE_CAST ((obj), LIVE_TYPE_GRAPH, LiveGraph))
#define LIVE_IS_GRAPH(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), LIVE_TYPE_GRAPH))

GType live_graph_get_type (void) G_GNUC_CONST;

G_DEFINE_TYPE (LiveGraph, live_graph, GTK_TYPE_WIDGET)

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static gboolean live_graph_is_dark_requested (LiveGraph *graph);
static void     live_graph_apply_theme (LiveGraph *graph, gboolean dark);
static void     live_graph_resolve_theme (LiveGraph *graph);

GtkWidget *
live_graph_new (gint max_samples)
{
    LiveGraph *graph;

    if (max_samples < 2)
        max_samples = 2;
    graph = g_object_new (LIVE_TYPE_GRAPH, NULL);

    graph->max_samples = max_samples;
    graph->samples = g_malloc0 (LG_N_SIGNALS * max_samples * sizeof (gdouble));
    graph->sample_time = g_malloc0 (max_samples * sizeof (gdouble));

    /* Display the full ring buffer by default. */
    graph->window = max_samples;

    return GTK_WIDGET (graph);
}

void
live_graph_push (LiveGraph *graph, gint signal, gdouble value)
{
    g_return_if_fail (LIVE_IS_GRAPH (graph));
    g_return_if_fail (signal >= 0 && signal < LG_N_SIGNALS);

    /* Store the real value as-is; only guard against non-finite input.
     * Avoid C99 isnan()/isinf(): use self-comparison (NaN != NaN) and a
     * magnitude check (|x| > DBL_MAX => infinity), both valid in C89. */
    if (value != value || (value > DBL_MAX || value < -DBL_MAX))
        value = 0.0;

    graph->samples[signal * graph->max_samples + graph->write_pos] = value;

    /* Advance the write position only after the last signal is pushed. */
    if (signal == LG_N_SIGNALS - 1) {
        graph->sample_time[graph->write_pos] =
            (gdouble) g_get_monotonic_time () / 1000000.0 - graph->t0;
        graph->write_pos = (graph->write_pos + 1) % graph->max_samples;
        if (graph->count < graph->max_samples)
            graph->count++;
        gtk_widget_queue_draw (GTK_WIDGET (graph));
    }
}

gint
live_graph_n_signals (void)
{
    return LG_N_SIGNALS;
}

void
live_graph_clear (LiveGraph *graph)
{
    g_return_if_fail (LIVE_IS_GRAPH (graph));
    graph->count = 0;
    graph->write_pos = 0;
    gtk_widget_queue_draw (GTK_WIDGET (graph));
}

void
live_graph_set_window (LiveGraph *graph, gint window)
{
    g_return_if_fail (LIVE_IS_GRAPH (graph));
    if (window < 2)
        window = 2;
    if (window > graph->max_samples)
        window = graph->max_samples;
    graph->window = window;
    gtk_widget_queue_draw (GTK_WIDGET (graph));
}

void
live_graph_set_theme (LiveGraph *graph, LiveGraphTheme theme)
{
    g_return_if_fail (LIVE_IS_GRAPH (graph));
    /*if (theme < LIVE_GRAPH_THEME_AUTO || theme > LIVE_GRAPH_THEME_LIGHT) theme = LIVE_GRAPH_THEME_AUTO;*/
    graph->theme = theme;
    live_graph_apply_theme (graph, live_graph_is_dark_requested (graph));
    gtk_widget_queue_draw (GTK_WIDGET (graph));
}

/* Select a signal line to draw thicker for emphasis; pass -1 for none. */
void
live_graph_select_line (LiveGraph *graph, gint signal)
{
    g_return_if_fail (LIVE_IS_GRAPH (graph));

    if (signal < 0)
        signal = -1;
    else if (signal >= LG_N_SIGNALS)
        signal = LG_N_SIGNALS - 1;

    if (graph->selected_line != signal) {
        graph->selected_line = signal;
        gtk_widget_queue_draw (GTK_WIDGET (graph));
    }
}

/* ------------------------------------------------------------------ */
/* Shared drawing helpers                                              */
/* ------------------------------------------------------------------ */

static void
lg_set_source (cairo_t *cr, const LgRGBA *c)
{
    cairo_set_source_rgba (cr, c->r, c->g, c->b, c->a);
}

static void
lg_set_color (LgRGBA *c, double r, double g, double b, double a)
{
    c->r = r; c->g = g; c->b = b; c->a = a;
}

static void
lg_hline (cairo_t *cr, const LgRGBA *c, gdouble x1, gdouble x2, gdouble y)
{
    lg_set_source (cr, c);
    cairo_set_line_width (cr, 1.0);
    cairo_move_to (cr, x1, y);
    cairo_line_to (cr, x2, y);
    cairo_stroke (cr);
}

/* Format a value with a compact SI suffix:
 *   K = 1e3, M = 1e6, G = 1e9, T = 1e12.
 * Magnitudes below 1e3 are printed as-is. */
static char *
lg_format_value (gdouble value)
{
    static char buf[32];
    gdouble av = value < 0 ? -value : value;
    const char *units[] = { "", "K", "M", "G", "T" };
    double scales[] = { 1.0, 1e3, 1e6, 1e9, 1e12 };
    int level = 0;

    if (av >= 1e12) {
        level = 4;
    } else if (av >= 1e9) {
        level = 3;
    } else if (av >= 1e6) {
        level = 2;
    } else if (av >= 1e3) {
        level = 1;
    }

    g_snprintf (buf, sizeof buf, "%.3g%s", value / scales[level], units[level]);
    return buf;
}

/* Format an elapsed time in seconds, e.g. "2.5s", "1m05s", "2h".
 * Negative values indicate "age" (how old a sample is) and render with a
 * leading minus, e.g. "-2.5s". */
static char *
lg_format_time (gdouble seconds)
{
    static char buf[32];
    const char *sign = "";
    gdouble abs_s;
    gint total;

    if (seconds < 0.0) {
        sign = "-";
        abs_s = -seconds;
    } else {
        abs_s = seconds;
    }

    if (abs_s < 60.0) {
        g_snprintf (buf, sizeof buf, "%s%.1fs", sign, abs_s);
    } else {
        gint mn, hr, s;
        total = (gint) abs_s;
        mn = total / 60;
        hr = mn / 60;
        mn %= 60;
        s = total % 60;
        if (hr > 0)
            g_snprintf (buf, sizeof buf, "%s%dh%02d", sign, hr, mn);
        else if (mn > 0)
            g_snprintf (buf, sizeof buf, "%s%dm%02d", sign, mn, s);
    }
    return buf;
}

static void
lg_render (LiveGraph *graph, cairo_t *cr, gint w, gint h)
{
    gdouble pad = 8.0;          /* outer padding (right/bottom + inner) */
    gdouble axis_w = 42.0;      /* width reserved for Y-axis labels      */
    gdouble axis_bottom = 18.0; /* height reserved for X-axis time labels */
    gdouble plot_x = axis_w;    /* plot area starts after the axis       */
    gdouble plot_y = pad;
    gdouble plot_w = (gdouble) w - axis_w - pad;
    gdouble plot_h = (gdouble) h - pad - axis_bottom;
    const LgRGBA *grid;
    const LgRGBA *border;
    const LgRGBA *label_c;
    LgRGBA *lines;
    gint n;
    gint start;
    gboolean have;
    gdouble vmin, vmax;
    gdouble vspan;
    gdouble *xs;
    int i, s, k;
    if (plot_w < 1 || plot_h < 1)
        return;

    /* Resolve the theme (auto-follows the app) and pick active colors. */
    live_graph_resolve_theme (graph);
    grid = &graph->grid_color;
    border = &graph->border_color;
    label_c = &graph->label_color;
    lines = graph->line_colors;
    if (!lines)
        lines = graph->line_colors_dark;

    /* Background. */
    lg_set_source (cr, &graph->bg_color);
    cairo_paint (cr);

    n = graph->count;
    if (n > graph->window)
        n = graph->window;
    n = n < 1 ? 1 : n;

    start = graph->write_pos - n;
    if (start < 0)
        start += graph->max_samples;

    /* ---- Autoscale: min/max over the visible window (all signals). ----
     * NoValue samples are excluded so the scale tracks the real data. */
    have = FALSE;
    vmin = 0.0;
    vmax = 0.0;
    for (s = 0; s < LG_N_SIGNALS; s++) {
        gint idx;
        gdouble v;
        for (i = 0; i < n; i++) {
            idx = (start + i) % graph->max_samples;
            v = graph->samples[s * graph->max_samples + idx];
            if (v == LG_NO_VALUE)
                continue;
            if (!have) {
                vmin = vmax = v;
                have = TRUE;
            } else {
                if (v < vmin) vmin = v;
                if (v > vmax) vmax = v;
            }
        }
    }

    /* Expand to a nice, stable range and guard against a flat signal. */
    if (!have) {
        vmin = 0.0;
        vmax = 1.0;
    } else {
        gdouble lo = vmin;
        gdouble hi = vmax;
        gdouble span = hi - lo;
        /* Pad by 10% on each side so peaks aren't glued to the edges. */
        if (span < 1e-12) {
            gdouble m = 0.5 * (lo + hi);
            span = fabs (m) > 1e-12 ? fabs (m) : 1.0;
            lo = m - span;
            hi = m + span;
        } else {
            lo -= 0.1 * span;
            hi += 0.1 * span;
        }
        vmin = lo;
        vmax = hi;
    }
    vspan = vmax - vmin;
    if (vspan < 1e-12)
        vspan = 1.0;

    /* ---- Y-axis labels + grid lines from the autoscaled range. ---- */
    cairo_select_font_face (cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size (cr, 10.0);
    for (i = 0; i <= 4; i++) {
        gdouble f = (gdouble) i / 4.0;
        gdouble gy = plot_y + plot_h * (1.0 - f);
        gdouble value = vmin + f * vspan;
        const char *label;
        cairo_text_extents_t te;

        /* Horizontal grid line across the plot. */
        lg_hline (cr, grid, plot_x, plot_x + plot_w, gy);

        /* Value label left of the plot, vertically centered on the grid. */
        label = lg_format_value (value);
        lg_set_source (cr, label_c);
        cairo_text_extents (cr, label, &te);
        cairo_move_to (cr, plot_x - te.width - 4, gy + te.height / 2.0);
        cairo_show_text (cr, label);
    }

    /* Build x positions once (same for every signal). */
    xs = g_newa (gdouble, n);
    for (i = 0; i < n; i++) {
        gdouble f = n > 1 ? (gdouble) i / (gdouble) (n - 1) : 0.5;
        xs[i] = plot_x + f * plot_w;
    }

    /* Draw each signal as its own polyline in its own color. */
    cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);

    for (s = 0; s < LG_N_SIGNALS; s++) {
        gboolean in_segment = FALSE;

        /* The selected line is drawn thicker for emphasis. */
        cairo_set_line_width (cr, s == graph->selected_line ? 3.2 : 1.6);

        lg_set_source (cr, &lines[s]);
        cairo_new_path (cr);
        for (i = 0; i < n; i++) {
            gint idx = (start + i) % graph->max_samples;
            gdouble v = graph->samples[s * graph->max_samples + idx];

            /* NoValue: break the line at this sample (no segment through it). */
            if (v == LG_NO_VALUE) {
                in_segment = FALSE;
                continue;
            }

            {
                gdouble f = (v - vmin) / vspan;
                gdouble y = plot_y + (1.0 - f) * plot_h;

                if (in_segment)
                    cairo_line_to (cr, xs[i], y);
                else {
                    cairo_move_to (cr, xs[i], y);
                    in_segment = TRUE;
                }
            }
        }
        cairo_stroke (cr);
    }

    /* Border around the plot area (excludes the axis labels). */
    lg_set_source (cr, border);
    cairo_set_line_width (cr, 1.0);
    cairo_rectangle (cr, plot_x, plot_y, plot_w, plot_h);
    cairo_stroke (cr);

    /* ---- X-axis time labels (sample age along the bottom). ----
     * Labels show how old each sample is: 0s / 0.5s at the newest (right)
     * and negative values (e.g. -5.0s) going left to the oldest sample. */
    {
        gint newest = (start + n - 1) % graph->max_samples;
        gdouble t_newest = graph->sample_time
                         ? graph->sample_time[newest] : 0.0;
        static const gdouble fracs[] = { 0.0, 0.5, 1.0 };
        cairo_select_font_face (cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
                                CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size (cr, 10.0);

        for (k = 0; k < 3; k++) {
            gdouble f = fracs[k];
            gint i2 = (gint) (f * (n - 1));
            gint idx = (start + i2) % graph->max_samples;
            gdouble age = graph->sample_time
                        ? graph->sample_time[idx] - t_newest : 0.0;
            const char *txt;
            gdouble tx;
            cairo_text_extents_t te;
            txt = lg_format_time (age);
            tx = plot_x + f * plot_w;
            cairo_text_extents (cr, txt, &te);

            /* Center the label at tx; clamp so it stays in the plot. */
            if (k == 0)      tx = plot_x + 2;
            else if (k == 2) tx = plot_x + plot_w - te.width - 2;
            else             tx -= te.width / 2.0;

            lg_set_source (cr, label_c);
            cairo_move_to (cr, tx, plot_y + plot_h + 13);
            cairo_show_text (cr, txt);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Theme support                                                       */
/* ------------------------------------------------------------------ */

/* Decide whether the widget should render in dark mode. For AUTO this
 * follows the application's color scheme preference. */
static gboolean
live_graph_is_dark_requested (LiveGraph *graph)
{
    gboolean dark = FALSE;

    if (graph->theme == LIVE_GRAPH_THEME_DARK)
        return TRUE;
    if (graph->theme == LIVE_GRAPH_THEME_LIGHT)
        return FALSE;

    /* AUTO: honor the application's dark-mode preference. */
#if defined(LG_GTK3) || defined(LG_GTK4)
    g_object_get (gtk_settings_get_default (),
                  "gtk-application-prefer-dark-theme", &dark, NULL);
    return !!dark;
#else
    /* GTK2 has no prefer-dark setting; infer from the active theme name. */
    {
        gchar *theme = NULL;
        g_object_get (gtk_settings_get_default (), "gtk-theme-name",
                      &theme, NULL);
        if (theme) {
            gchar *lower = g_ascii_strdown (theme, -1);
            dark = (strstr (lower, "-dark") || strstr (lower, "dark"));
            g_free (lower);
            g_free (theme);
        }
    }
    return dark;
#endif
}

/* Populate the active colors (bg, grid, border, line palette) from the
 * resolved theme. */
static void
live_graph_apply_theme (LiveGraph *graph, gboolean dark)
{
    if (dark) {
        lg_set_color (&graph->bg_color,    0.10, 0.10, 0.12, 1.0);
        lg_set_color (&graph->grid_color,  0x46/255.0, 0x4a/255.0, 0x51/255.0, 1.0);
        lg_set_color (&graph->border_color,0x5a/255.0, 0x5e/255.0, 0x66/255.0, 1.0);
        lg_set_color (&graph->label_color, 1.0, 1.0, 1.0, 1.0);   /* white */
        graph->line_colors = graph->line_colors_dark;
    } else {
        lg_set_color (&graph->bg_color,    0.97, 0.97, 0.97, 1.0);
        lg_set_color (&graph->grid_color,  0xbf/255.0, 0xc4/255.0, 0xcc/255.0, 1.0);
        lg_set_color (&graph->border_color,0x60/255.0, 0x64/255.0, 0x6b/255.0, 1.0);
        lg_set_color (&graph->label_color, 0.0, 0.0, 0.0, 1.0);   /* black */
        graph->line_colors = graph->line_colors_light;
    }
}

static void
live_graph_resolve_theme (LiveGraph *graph)
{
    live_graph_apply_theme (graph, live_graph_is_dark_requested (graph));
}

/* ------------------------------------------------------------------ */
/* Widget class implementation                                         */
/* ------------------------------------------------------------------ */

static void
live_graph_finalize (GObject *object)
{
    LiveGraph *graph = LIVE_GRAPH (object);

    g_free (graph->samples);
    g_free (graph->sample_time);
    G_OBJECT_CLASS (live_graph_parent_class)->finalize (object);
}

static void
live_graph_init (LiveGraph *graph)
{
    int s;

    /* Line colors tuned for a dark background (bright/saturated hues). */
    static const double dark_palette[LG_N_SIGNALS][3] = {
        { 0.31, 0.76, 0.97 },   /* bright blue   */
        { 1.00, 0.62, 0.22 },   /* orange        */
        { 0.30, 0.85, 0.39 },   /* green         */
        { 0.93, 0.32, 0.34 },   /* red           */
        { 0.68, 0.47, 0.98 },   /* violet        */
        { 0.24, 0.86, 0.81 },   /* teal          */
        { 0.98, 0.80, 0.24 },   /* yellow        */
        { 1.00, 0.41, 0.70 },   /* pink          */
        { 0.55, 0.78, 0.30 },   /* lime          */
        { 0.95, 0.60, 0.30 }    /* amber         */
    };
    /* Line colors tuned for a light background (darker for contrast). */
    static const double light_palette[LG_N_SIGNALS][3] = {
        { 0.06, 0.42, 0.74 },   /* deep blue     */
        { 0.82, 0.35, 0.04 },   /* burnt orange  */
        { 0.06, 0.55, 0.18 },   /* deep green    */
        { 0.78, 0.08, 0.12 },   /* deep red      */
        { 0.42, 0.16, 0.70 },   /* deep violet   */
        { 0.00, 0.45, 0.45 },   /* dark teal     */
        { 0.62, 0.47, 0.00 },   /* dark yellow   */
        { 0.72, 0.10, 0.42 },   /* deep pink     */
        { 0.34, 0.52, 0.08 },   /* olive         */
        { 0.62, 0.34, 0.04 }    /* dark amber    */
    };

    graph->write_pos = 0;
    graph->count = 0;
    graph->window = 1024;       /* overridden by the constructor capacity */
    graph->selected_line = -1;

    graph->max_samples = 1024;
    graph->samples = NULL;
    graph->sample_time = NULL;
    graph->t0 = (gdouble) g_get_monotonic_time () / 1000000.0;

    graph->theme = LIVE_GRAPH_THEME_AUTO;
    graph->line_colors = graph->line_colors_dark;

    for (s = 0; s < LG_N_SIGNALS; s++) {
        lg_set_color (&graph->line_colors_dark[s],
                      dark_palette[s][0], dark_palette[s][1], dark_palette[s][2], 1.0);
        lg_set_color (&graph->line_colors_light[s],
                      light_palette[s][0], light_palette[s][1], light_palette[s][2], 1.0);
    }

    live_graph_apply_theme (graph, TRUE);

#if defined(LG_GTK2)
    gtk_widget_set_has_window (GTK_WIDGET (graph), TRUE);
#elif defined(LG_GTK3)
    gtk_widget_set_has_window (GTK_WIDGET (graph), FALSE);
#endif
}

#if defined(LG_GTK4)
/* ---------------- GTK4 ---------------- */

static void
live_graph_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
    LiveGraph *graph = LIVE_GRAPH (widget);
    graphene_rect_t bounds;
    cairo_t *cr;

    bounds = GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget),
                                 gtk_widget_get_height (widget));
    cr = gtk_snapshot_append_cairo (snapshot, &bounds);
    lg_render (graph, cr, (gint) bounds.size.width, (gint) bounds.size.height);
    cairo_destroy (cr);
}

static void
live_graph_measure (GtkWidget *widget,
                    GtkOrientation orientation,
                    gint for_size,
                    gint *minimum,
                    gint *natural,
                    gint *minimum_baseline,
                    gint *natural_baseline)
{
    (void) widget;
    (void) orientation;
    (void) for_size;
    *minimum = 60;
    *natural = 260;
    if (minimum_baseline)
        *minimum_baseline = -1;
    if (natural_baseline)
        *natural_baseline = -1;
}

static void
live_graph_class_init (LiveGraphClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    gobject_class->finalize = live_graph_finalize;
    widget_class->snapshot = live_graph_snapshot;
    widget_class->measure = live_graph_measure;
}

#else
/* ---------------- GTK2 / GTK3 ---------------- */

#if defined(LG_GTK3)
static gboolean
live_graph_draw (GtkWidget *widget, cairo_t *cr)
{
    LiveGraph *graph = LIVE_GRAPH (widget);
    lg_render (graph, cr, gtk_widget_get_allocated_width (widget),
               gtk_widget_get_allocated_height (widget));
    return FALSE;
}
#else /* GTK2 */
/* GTK2 draws a custom widget on its own GdkWindow, like GtkDrawingArea:
 * gdk_cairo_create(widget->window) is then in widget-local coordinates, so
 * no translation is needed and nothing is clipped or offset. */
static gboolean
live_graph_expose (GtkWidget *widget, GdkEventExpose *event)
{
    LiveGraph *graph = LIVE_GRAPH (widget);
    cairo_t *cr;
    GtkAllocation alloc;

    if (!gtk_widget_is_drawable (widget))
        return FALSE;

    gtk_widget_get_allocation (widget, &alloc);

    cr = gdk_cairo_create (widget->window);
    if (event->region)
        gdk_cairo_region (cr, event->region);
    lg_render (graph, cr, alloc.width, alloc.height);
    cairo_destroy (cr);
    return FALSE;
}

static void
live_graph_realize (GtkWidget *widget)
{
    GdkWindowAttr attributes;
    gint attributes_mask;

    gtk_widget_set_realized (widget, TRUE);

    attributes.window_type = GDK_WINDOW_CHILD;
    attributes.x = widget->allocation.x;
    attributes.y = widget->allocation.y;
    attributes.width = widget->allocation.width;
    attributes.height = widget->allocation.height;
    attributes.wclass = GDK_INPUT_OUTPUT;
    attributes.visual = gtk_widget_get_visual (widget);
    attributes.colormap = gtk_widget_get_colormap (widget);
    attributes.event_mask = gtk_widget_get_events (widget) | GDK_EXPOSURE_MASK;

    attributes_mask = GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL | GDK_WA_COLORMAP;

    widget->window = gdk_window_new (gtk_widget_get_parent_window (widget),
                                     &attributes, attributes_mask);
    gdk_window_set_user_data (widget->window, widget);

    widget->style = gtk_style_attach (widget->style, widget->window);
    gtk_style_set_background (widget->style, widget->window, GTK_STATE_NORMAL);
}

static void
live_graph_size_allocate (GtkWidget *widget, GtkAllocation *alloc)
{
    widget->allocation = *alloc;

    if (gtk_widget_get_realized (widget))
        gdk_window_move_resize (widget->window,
                                alloc->x, alloc->y,
                                alloc->width, alloc->height);
}
#endif

#if defined(LG_GTK2)
static void
live_graph_size_request (GtkWidget *widget, GtkRequisition *req)
{
    GTK_WIDGET_CLASS (live_graph_parent_class)->size_request (widget, req);
    if (req->width < 200)
        req->width = 200;
    if (req->height < 120)
        req->height = 120;
}
#else
static void
live_graph_get_preferred_width (GtkWidget *widget, gint *minimum, gint *natural)
{
    (void) widget;
    *minimum = 120;
    *natural = 260;
}

static void
live_graph_get_preferred_height (GtkWidget *widget, gint *minimum, gint *natural)
{
    (void) widget;
    *minimum = 80;
    *natural = 160;
}
#endif

static void
live_graph_class_init (LiveGraphClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    gobject_class->finalize = live_graph_finalize;

#if defined(LG_GTK3)
    widget_class->draw = live_graph_draw;
    widget_class->get_preferred_width = live_graph_get_preferred_width;
    widget_class->get_preferred_height = live_graph_get_preferred_height;
#else /* GTK2 */
    widget_class->expose_event = live_graph_expose;
    widget_class->size_request = live_graph_size_request;
    widget_class->realize = live_graph_realize;
    widget_class->size_allocate = live_graph_size_allocate;
#endif
}

#endif
