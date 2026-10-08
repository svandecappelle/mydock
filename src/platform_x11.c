/* Platform services on X11. */

#include "platform.h"

#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <gdk/gdkx.h>
#include <glib-unix.h>
#include <signal.h>

void platform_setup_dock_window(GtkWidget *window)
{
    /* GTK's DOCK type hint, keep-above and sticky flags are all X11 needs. */
}

gboolean platform_input_follows_alpha(void)
{
    return FALSE; /* the X input shape decides */
}

/* GTK3 has no API for this: set _NET_WM_STRUT_PARTIAL (and the older
 * _NET_WM_STRUT) on the window. */
void platform_reserve_space(GtkWidget *window, DockPosition position, const GdkRectangle *g, int thickness)
{
    GdkWindow *gdk_window = gtk_widget_get_window(window);
    if (!gdk_window)
        return;
    Display *dpy = GDK_WINDOW_XDISPLAY(gdk_window);
    Window xid = GDK_WINDOW_XID(gdk_window);
    if (thickness <= 0) {
        XDeleteProperty(dpy, xid, XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False));
        XDeleteProperty(dpy, xid, XInternAtom(dpy, "_NET_WM_STRUT", False));
        return;
    }
    /* Struts are relative to the edges of the whole screen, not the monitor. */
    GdkDisplay *display = gdk_window_get_display(gdk_window);
    long screen_w = 0, screen_h = 0;
    for (int i = 0; i < gdk_display_get_n_monitors(display); i++) {
        GdkRectangle m;
        gdk_monitor_get_geometry(gdk_display_get_monitor(display, i), &m);
        screen_w = MAX(screen_w, m.x + m.width);
        screen_h = MAX(screen_h, m.y + m.height);
    }
    long sf = gtk_widget_get_scale_factor(window);
    long thick = thickness;
    /* left, right, top, bottom, then start/end pairs for each of them */
    long partial[12] = { 0 };
    switch (position) {
    case DOCK_TOP:
        partial[2] = g->y + thick;
        partial[8] = g->x, partial[9] = g->x + g->width - 1;
        break;
    case DOCK_LEFT:
        partial[0] = g->x + thick;
        partial[4] = g->y, partial[5] = g->y + g->height - 1;
        break;
    case DOCK_RIGHT:
        partial[1] = screen_w - (g->x + g->width) + thick;
        partial[6] = g->y, partial[7] = g->y + g->height - 1;
        break;
    default:
        partial[3] = screen_h - (g->y + g->height) + thick;
        partial[10] = g->x, partial[11] = g->x + g->width - 1;
        break;
    }
    for (int i = 0; i < 12; i++)
        partial[i] *= sf;
    XChangeProperty(dpy, xid, XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False), XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)partial, 12);
    XChangeProperty(dpy, xid, XInternAtom(dpy, "_NET_WM_STRUT", False), XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)partial, 4);
}

/* Set the X window's bounding shape directly (X Shape extension): GDK keeps
 * its own idea of a toplevel's shape and can reset what
 * gtk_widget_shape_combine_region() set (seen right after startup), so
 * leave GDK out of it. */
void platform_set_window_shape(GtkWidget *window, const cairo_region_t *region)
{
    GdkWindow *gdk_window = gtk_widget_get_window(window);
    if (!gdk_window)
        return;
    int sf = gtk_widget_get_scale_factor(window);
    int n = cairo_region_num_rectangles(region);
    XRectangle *rects = g_new(XRectangle, MAX(n, 1));
    for (int i = 0; i < n; i++) {
        cairo_rectangle_int_t r;
        cairo_region_get_rectangle(region, i, &r);
        rects[i] = (XRectangle){ r.x * sf, r.y * sf, r.width * sf, r.height * sf };
    }
    XShapeCombineRectangles(GDK_WINDOW_XDISPLAY(gdk_window), GDK_WINDOW_XID(gdk_window), ShapeBounding,
                            0, 0, rects, n, ShapeSet, Unsorted);
    g_free(rects);
}

void platform_on_quit_request(GSourceFunc func, gpointer data)
{
    g_unix_signal_add(SIGINT, func, data);
    g_unix_signal_add(SIGTERM, func, data);
}
