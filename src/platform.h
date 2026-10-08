#pragma once

#include <gtk/gtk.h>

#include "config.h"

/* What the dock window needs from the platform beyond GTK, implemented per
 * platform (platform_x11.c). */

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
