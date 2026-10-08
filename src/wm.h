#pragma once

#include <gtk/gtk.h>

/* Window tracking: the windows that belong in a task list, and the actions
 * the dock performs on them. Implemented per platform (wm_wnck.c on X11). */

/* A top-level application window. Opaque handle owned by the backend, valid
 * until the `closed` callback has been called for it. */
typedef struct WmWindow WmWindow;

typedef struct {
    /* A window that belongs in a task list appeared, or stopped being
     * hidden from it; `closed` is the opposite. */
    void (*opened)(WmWindow *window, gpointer data);
    void (*closed)(WmWindow *window, gpointer data);
    /* The application the window claims to belong to changed. */
    void (*app_changed)(WmWindow *window, gpointer data);
    void (*icon_changed)(WmWindow *window, gpointer data);
    void (*active_changed)(gpointer data);
    gpointer data;
} WmCallbacks;

typedef struct Wm Wm;

/* Starts tracking; the callbacks only report later changes. */
Wm *wm_new(const WmCallbacks *callbacks);
/* Tracked windows in the order they were opened; free with g_list_free. */
GList *wm_get_windows(Wm *wm);
/* Tracked windows, bottom-most first; free with g_list_free. */
GList *wm_get_windows_stacked(Wm *wm);
WmWindow *wm_get_active_window(Wm *wm); /* NULL if none, or not tracked */

const char *wm_window_get_title(WmWindow *window);
/* Names identifying the window's application, most specific first (X11:
 * WM_CLASS instance and class). Either may be NULL or empty. */
const char *wm_window_get_app_id(WmWindow *window);
const char *wm_window_get_app_group(WmWindow *window);
int wm_window_get_pid(WmWindow *window);
/* The window's own icon, or NULL if it has none (owned by the window). */
GdkPixbuf *wm_window_get_icon(WmWindow *window);
gboolean wm_window_is_minimized(WmWindow *window);

/* Raises and focuses the window, switching workspace if needed. */
void wm_window_activate(Wm *wm, WmWindow *window, guint32 timestamp);
void wm_window_minimize(WmWindow *window);
void wm_window_unminimize(WmWindow *window, guint32 timestamp);
void wm_window_close(WmWindow *window, guint32 timestamp);
