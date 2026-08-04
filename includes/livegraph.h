/*
 * livegraph.h - A live-updating graph widget for GTK 2, 3 and 4.
 *
 * Compiles against GTK2, GTK3 or GTK4 from the same source using the
 * version macros in the installed headers. Provides a custom widget that
 * keeps a ring buffer of samples and redraws itself as data is pushed.
 *
 * Written by: hwspeedy
 * License: GPL2+
 *
 */

#ifndef __LIVEGRAPH_H__
#define __LIVEGRAPH_H__

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _LiveGraph LiveGraph;

/* Sentinel: push this value for a signal to hide/leave that line blank. */
#define LG_NO_VALUE DBL_MAX

/* Theme selection for the graph widget. */
typedef enum {
    LIVE_GRAPH_THEME_AUTO,   /* follow the application/system theme */
    LIVE_GRAPH_THEME_DARK,
    LIVE_GRAPH_THEME_LIGHT
} LiveGraphTheme;

#define LIVE_TYPE_GRAPH    (live_graph_get_type ())
#define LIVE_GRAPH(obj)    (G_TYPE_CHECK_INSTANCE_CAST ((obj), LIVE_TYPE_GRAPH, LiveGraph))
#define LIVE_IS_GRAPH(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), LIVE_TYPE_GRAPH))

GType  live_graph_get_type (void) G_GNUC_CONST;

/* Create a graph with an explicit ring-buffer capacity (max number of
 * samples retained per signal). The capacity is fixed for the lifetime of
 * the widget. */
GtkWidget *live_graph_new (gint max_samples);

/* Push a raw sample for the given signal index (autoscaled internally). */
void live_graph_push (LiveGraph *graph, gint signal, gdouble value);

gint live_graph_n_signals (void);

void live_graph_clear (LiveGraph *graph);

void live_graph_set_window (LiveGraph *graph, gint window);

/* Set the color theme: AUTO follows the application/system preference. */
void live_graph_set_theme (LiveGraph *graph, LiveGraphTheme theme);

/* Select a signal line to draw thicker for emphasis; pass -1 for none. */
void live_graph_select_line (LiveGraph *graph, gint signal);

G_END_DECLS

#endif  /* __LIVEGRAPH_H__ */
