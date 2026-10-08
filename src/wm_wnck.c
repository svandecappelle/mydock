/* Window tracking on X11, through libwnck. */

#include "wm.h"

#include <libwnck/libwnck.h>

#define WNCK(w) ((WnckWindow *)(w))
#define WM_WINDOW(w) ((WmWindow *)(w))

struct Wm {
    WmCallbacks cb;
    WnckHandle *handle;
    WnckScreen *screen;
    GHashTable *tracked; /* WnckWindow* set: the windows reported as opened */
};

static gboolean belongs_in_tasklist(WnckWindow *w)
{
    WnckWindowType type = wnck_window_get_window_type(w);
    return (type == WNCK_WINDOW_NORMAL || type == WNCK_WINDOW_DIALOG) && !wnck_window_is_skip_tasklist(w);
}

/* Report the window as opened or closed if it entered or left the task list. */
static void update_tracked(Wm *wm, WnckWindow *w, gboolean notify)
{
    gboolean tracked = g_hash_table_contains(wm->tracked, w);
    if (belongs_in_tasklist(w) && !tracked) {
        g_hash_table_add(wm->tracked, w);
        if (notify)
            wm->cb.opened(WM_WINDOW(w), wm->cb.data);
    } else if (!belongs_in_tasklist(w) && tracked) {
        g_hash_table_remove(wm->tracked, w);
        if (notify)
            wm->cb.closed(WM_WINDOW(w), wm->cb.data);
    }
}

static void on_class_changed(WnckWindow *w, Wm *wm)
{
    if (g_hash_table_contains(wm->tracked, w))
        wm->cb.app_changed(WM_WINDOW(w), wm->cb.data);
}

static void on_state_changed(WnckWindow *w, WnckWindowState changed, WnckWindowState state, Wm *wm)
{
    if (changed & WNCK_WINDOW_STATE_SKIP_TASKLIST)
        update_tracked(wm, w, TRUE);
}

static void on_icon_changed(WnckWindow *w, Wm *wm)
{
    if (g_hash_table_contains(wm->tracked, w))
        wm->cb.icon_changed(WM_WINDOW(w), wm->cb.data);
}

static void watch_window(Wm *wm, WnckWindow *w, gboolean notify)
{
    g_signal_connect(w, "class-changed", G_CALLBACK(on_class_changed), wm);
    g_signal_connect(w, "state-changed", G_CALLBACK(on_state_changed), wm);
    g_signal_connect(w, "icon-changed", G_CALLBACK(on_icon_changed), wm);
    update_tracked(wm, w, notify);
}

static void on_window_opened(WnckScreen *screen, WnckWindow *w, Wm *wm)
{
    watch_window(wm, w, TRUE);
}

static void on_window_closed(WnckScreen *screen, WnckWindow *w, Wm *wm)
{
    if (g_hash_table_remove(wm->tracked, w))
        wm->cb.closed(WM_WINDOW(w), wm->cb.data);
}

static void on_active_window_changed(WnckScreen *screen, WnckWindow *previous, Wm *wm)
{
    wm->cb.active_changed(wm->cb.data);
}

Wm *wm_new(const WmCallbacks *callbacks)
{
    Wm *wm = g_new0(Wm, 1);
    wm->cb = *callbacks;
    wm->tracked = g_hash_table_new(g_direct_hash, g_direct_equal);
    wm->handle = wnck_handle_new(WNCK_CLIENT_TYPE_APPLICATION);
    wnck_handle_set_default_icon_size(wm->handle, 128);
    wm->screen = wnck_handle_get_default_screen(wm->handle);
    wnck_screen_force_update(wm->screen);
    for (GList *l = wnck_screen_get_windows(wm->screen); l; l = l->next)
        watch_window(wm, l->data, FALSE);
    g_signal_connect(wm->screen, "window-opened", G_CALLBACK(on_window_opened), wm);
    g_signal_connect(wm->screen, "window-closed", G_CALLBACK(on_window_closed), wm);
    g_signal_connect(wm->screen, "active-window-changed", G_CALLBACK(on_active_window_changed), wm);
    return wm;
}

static GList *tracked_only(Wm *wm, GList *windows)
{
    GList *result = NULL;
    for (GList *l = windows; l; l = l->next)
        if (g_hash_table_contains(wm->tracked, l->data))
            result = g_list_prepend(result, l->data);
    return g_list_reverse(result);
}

GList *wm_get_windows(Wm *wm)
{
    return tracked_only(wm, wnck_screen_get_windows(wm->screen));
}

GList *wm_get_windows_stacked(Wm *wm)
{
    return tracked_only(wm, wnck_screen_get_windows_stacked(wm->screen));
}

WmWindow *wm_get_active_window(Wm *wm)
{
    WnckWindow *active = wnck_screen_get_active_window(wm->screen);
    return active && g_hash_table_contains(wm->tracked, active) ? WM_WINDOW(active) : NULL;
}

const char *wm_window_get_title(WmWindow *window)
{
    return wnck_window_get_name(WNCK(window));
}

const char *wm_window_get_app_id(WmWindow *window)
{
    return wnck_window_get_class_instance_name(WNCK(window));
}

const char *wm_window_get_app_group(WmWindow *window)
{
    return wnck_window_get_class_group_name(WNCK(window));
}

int wm_window_get_pid(WmWindow *window)
{
    return wnck_window_get_pid(WNCK(window));
}

GdkPixbuf *wm_window_get_icon(WmWindow *window)
{
    return wnck_window_get_icon_is_fallback(WNCK(window)) ? NULL : wnck_window_get_icon(WNCK(window));
}

gboolean wm_window_is_minimized(WmWindow *window)
{
    return wnck_window_is_minimized(WNCK(window));
}

void wm_window_activate(Wm *wm, WmWindow *window, guint32 timestamp)
{
    WnckWorkspace *ws = wnck_window_get_workspace(WNCK(window));
    if (ws && ws != wnck_screen_get_active_workspace(wm->screen))
        wnck_workspace_activate(ws, timestamp);
    wnck_window_activate_transient(WNCK(window), timestamp);
}

void wm_window_minimize(WmWindow *window)
{
    wnck_window_minimize(WNCK(window));
}

void wm_window_unminimize(WmWindow *window, guint32 timestamp)
{
    wnck_window_unminimize(WNCK(window), timestamp);
}

void wm_window_close(WmWindow *window, guint32 timestamp)
{
    wnck_window_close(WNCK(window), timestamp);
}
