#pragma once

#include <gtk/gtk.h>

#include "config.h"

/* What the dock window needs from the platform beyond GTK, implemented per
 * platform (platform_x11.c). */

/* Finish setting up the realized dock window beyond what GTK's hints do. */
void platform_setup_dock_window(GtkWidget *window);

/* TRUE if fully transparent pixels let the pointer through whatever the
 * input shape (Windows: per-pixel-alpha layered windows). The dock then
 * paints its input area with an invisible, almost transparent colour. */
gboolean platform_input_follows_alpha(void);

/* Keep a strip `thickness` px deep along the `position` edge of `monitor`
 * free of maximized windows, or stop doing so if `thickness` is 0. The
 * window must be realized. */
void platform_reserve_space(GtkWidget *window, DockPosition position, const GdkRectangle *monitor, int thickness);

/* Cut the window to `region` (window coordinates), so that what lies outside
 * it shows the windows below. Used when there is no compositor. */
void platform_set_window_shape(GtkWidget *window, const cairo_region_t *region);

/* Call `func` from the main loop when the system asks the process to quit
 * (POSIX: SIGINT, SIGTERM). */
void platform_on_quit_request(GSourceFunc func, gpointer data);
