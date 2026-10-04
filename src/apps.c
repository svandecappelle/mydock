#include "apps.h"

#include <string.h>

#define N_TIERS 3

struct AppTracker {
    DockConfig *cfg;
    AppTrackerCallbacks cb;
    WnckHandle *wnck;
    WnckScreen *screen;
    GPtrArray *items;            /* DockItem* (owned, freed by hand) */
    GHashTable *window_items;    /* WnckWindow* -> DockItem* */
    /* Lowercase name -> GDesktopAppInfo, in decreasing reliability:
     * StartupWMClass, desktop id (and its last dotted part), executable name. */
    GHashTable *index[N_TIERS];
};

/* ---- DockItem ------------------------------------------------------------ */

static DockItem *dock_item_new(const char *key, GDesktopAppInfo *info, gboolean pinned)
{
    DockItem *item = g_new0(DockItem, 1);
    item->key = g_strdup(key);
    item->app_info = info ? g_object_ref(info) : NULL;
    item->pinned = pinned;
    item->icons = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_object_unref);
    return item;
}

static void dock_item_free(DockItem *item)
{
    g_free(item->key);
    g_clear_object(&item->app_info);
    g_list_free(item->windows);
    g_hash_table_destroy(item->icons);
    g_free(item);
}

const char *dock_item_name(DockItem *item)
{
    if (item->app_info)
        return g_app_info_get_display_name(G_APP_INFO(item->app_info));
    if (item->windows) {
        WnckWindow *w = item->windows->data;
        const char *name = wnck_window_get_class_group_name(w);
        return name && *name ? name : wnck_window_get_name(w);
    }
    return item->key;
}

static GdkPixbuf *load_icon(DockItem *item, int size)
{
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    GIcon *gicon = item->app_info ? g_app_info_get_icon(G_APP_INFO(item->app_info)) : NULL;
    if (gicon) {
        GtkIconInfo *info = gtk_icon_theme_lookup_by_gicon(theme, gicon, size, GTK_ICON_LOOKUP_FORCE_SIZE);
        if (info) {
            GdkPixbuf *pixbuf = gtk_icon_info_load_icon(info, NULL);
            g_object_unref(info);
            if (pixbuf)
                return pixbuf;
        }
    }
    for (GList *l = item->windows; l; l = l->next)
        if (!wnck_window_get_icon_is_fallback(l->data))
            return g_object_ref(wnck_window_get_icon(l->data));

    GdkPixbuf *pixbuf = gtk_icon_theme_load_icon(theme, "application-x-executable", size,
                                                 GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    if (!pixbuf) {
        pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, size, size);
        gdk_pixbuf_fill(pixbuf, 0);
    }
    return pixbuf;
}

GdkPixbuf *dock_item_icon(DockItem *item, int size)
{
    GdkPixbuf *pixbuf = g_hash_table_lookup(item->icons, GINT_TO_POINTER(size));
    if (!pixbuf) {
        pixbuf = load_icon(item, size);
        g_hash_table_insert(item->icons, GINT_TO_POINTER(size), pixbuf);
    }
    return pixbuf;
}

void dock_item_clear_icons(DockItem *item)
{
    g_hash_table_remove_all(item->icons);
}

/* ---- window -> application matching -------------------------------------- */

static void index_add(GHashTable *table, const char *name, GDesktopAppInfo *info)
{
    char *key = g_ascii_strdown(name, -1);
    if (g_hash_table_contains(table, key))
        g_free(key);
    else
        g_hash_table_insert(table, key, g_object_ref(info));
}

static void build_index(AppTracker *t)
{
    for (int i = 0; i < N_TIERS; i++)
        g_hash_table_remove_all(t->index[i]);

    GList *all = g_app_info_get_all();
    for (GList *l = all; l; l = l->next) {
        if (!G_IS_DESKTOP_APP_INFO(l->data))
            continue;
        GDesktopAppInfo *info = l->data;

        const char *wm_class = g_desktop_app_info_get_startup_wm_class(info);
        if (wm_class)
            index_add(t->index[0], wm_class, info);

        const char *id = g_app_info_get_id(G_APP_INFO(info));
        if (id) {
            g_autofree char *base = g_str_has_suffix(id, ".desktop")
                ? g_strndup(id, strlen(id) - strlen(".desktop")) : g_strdup(id);
            index_add(t->index[1], base, info);
            const char *dot = strrchr(base, '.');
            if (dot)
                index_add(t->index[1], dot + 1, info);
        }

        const char *exe = g_app_info_get_executable(G_APP_INFO(info));
        if (exe) {
            g_autofree char *exe_name = g_path_get_basename(exe);
            index_add(t->index[2], exe_name, info);
        }
    }
    g_list_free_full(all, g_object_unref);
}

static GDesktopAppInfo *match_window(AppTracker *t, WnckWindow *w)
{
    const char *names[] = { wnck_window_get_class_instance_name(w), wnck_window_get_class_group_name(w) };
    for (int tier = 0; tier < N_TIERS; tier++) {
        for (guint n = 0; n < G_N_ELEMENTS(names); n++) {
            if (!names[n] || !*names[n])
                continue;
            g_autofree char *key = g_ascii_strdown(names[n], -1);
            GDesktopAppInfo *info = g_hash_table_lookup(t->index[tier], key);
            if (info)
                return info;
        }
    }
    return NULL;
}

static char *key_for(WnckWindow *w, GDesktopAppInfo *info)
{
    if (info)
        return g_strdup(g_app_info_get_id(G_APP_INFO(info)));
    const char *group = wnck_window_get_class_group_name(w);
    if (group && *group) {
        g_autofree char *lower = g_ascii_strdown(group, -1);
        return g_strconcat("wmclass:", lower, NULL);
    }
    return g_strdup_printf("wmclass:pid-%d", wnck_window_get_pid(w));
}

/* ---- window bookkeeping -------------------------------------------------- */

static gboolean is_tracked(WnckWindow *w)
{
    WnckWindowType type = wnck_window_get_window_type(w);
    return (type == WNCK_WINDOW_NORMAL || type == WNCK_WINDOW_DIALOG) && !wnck_window_is_skip_tasklist(w);
}

static DockItem *find_item(AppTracker *t, const char *key)
{
    for (guint i = 0; i < t->items->len; i++) {
        DockItem *item = g_ptr_array_index(t->items, i);
        if (g_str_equal(item->key, key))
            return item;
    }
    return NULL;
}

static void notify(AppTracker *t)
{
    t->cb.changed(t->cb.data);
}

static void remove_item(AppTracker *t, DockItem *item)
{
    g_ptr_array_remove(t->items, item);
    t->cb.item_removed(item, t->cb.data);
    dock_item_free(item);
}

static void add_window(AppTracker *t, WnckWindow *w, gboolean do_notify)
{
    if (!is_tracked(w) || g_hash_table_contains(t->window_items, w))
        return;
    GDesktopAppInfo *info = match_window(t, w);
    g_autofree char *key = key_for(w, info);
    DockItem *item = find_item(t, key);
    if (!item) {
        item = dock_item_new(key, info, FALSE);
        g_ptr_array_add(t->items, item);
    }
    item->windows = g_list_append(item->windows, w);
    g_hash_table_insert(t->window_items, w, item);
    if (do_notify)
        notify(t);
}

static void remove_window(AppTracker *t, WnckWindow *w, gboolean do_notify)
{
    DockItem *item = g_hash_table_lookup(t->window_items, w);
    if (!item)
        return;
    g_hash_table_remove(t->window_items, w);
    item->windows = g_list_remove(item->windows, w);
    if (!item->windows && !item->pinned)
        remove_item(t, item);
    if (do_notify)
        notify(t);
}

/* Re-file a window whose class or skip-tasklist state changed. */
static void on_window_changed(WnckWindow *w, AppTracker *t)
{
    DockItem *current = g_hash_table_lookup(t->window_items, w);
    if (is_tracked(w)) {
        g_autofree char *key = key_for(w, match_window(t, w));
        if (current && g_str_equal(current->key, key))
            return;
    } else if (!current) {
        return;
    }
    remove_window(t, w, FALSE);
    add_window(t, w, FALSE);
    notify(t);
}

static void on_window_state_changed(WnckWindow *w, WnckWindowState changed, WnckWindowState state, AppTracker *t)
{
    if (changed & WNCK_WINDOW_STATE_SKIP_TASKLIST)
        on_window_changed(w, t);
}

static void on_window_icon_changed(WnckWindow *w, AppTracker *t)
{
    DockItem *item = g_hash_table_lookup(t->window_items, w);
    if (item && !item->app_info) {
        dock_item_clear_icons(item);
        notify(t);
    }
}

static void on_window_opened(WnckScreen *screen, WnckWindow *w, AppTracker *t)
{
    g_signal_connect(w, "class-changed", G_CALLBACK(on_window_changed), t);
    g_signal_connect(w, "state-changed", G_CALLBACK(on_window_state_changed), t);
    g_signal_connect(w, "icon-changed", G_CALLBACK(on_window_icon_changed), t);
    add_window(t, w, TRUE);
}

static void on_window_closed(WnckScreen *screen, WnckWindow *w, AppTracker *t)
{
    remove_window(t, w, TRUE);
}

static void on_active_window_changed(WnckScreen *screen, WnckWindow *previous, AppTracker *t)
{
    notify(t);
}

static void on_apps_changed(GAppInfoMonitor *monitor, AppTracker *t)
{
    build_index(t);
}

AppTracker *app_tracker_new(DockConfig *cfg, const AppTrackerCallbacks *callbacks)
{
    AppTracker *t = g_new0(AppTracker, 1);
    t->cfg = cfg;
    t->cb = *callbacks;
    t->items = g_ptr_array_new();
    t->window_items = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (int i = 0; i < N_TIERS; i++)
        t->index[i] = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    build_index(t);
    g_signal_connect(g_app_info_monitor_get(), "changed", G_CALLBACK(on_apps_changed), t);

    for (char **id = cfg->pinned; *id; id++) {
        g_autoptr(GDesktopAppInfo) info = g_desktop_app_info_new(*id);
        if (info && !find_item(t, *id))
            g_ptr_array_add(t->items, dock_item_new(*id, info, TRUE));
    }

    t->wnck = wnck_handle_new(WNCK_CLIENT_TYPE_APPLICATION);
    wnck_handle_set_default_icon_size(t->wnck, 128);
    t->screen = wnck_handle_get_default_screen(t->wnck);
    wnck_screen_force_update(t->screen);
    for (GList *l = wnck_screen_get_windows(t->screen); l; l = l->next) {
        g_signal_connect(l->data, "class-changed", G_CALLBACK(on_window_changed), t);
        g_signal_connect(l->data, "state-changed", G_CALLBACK(on_window_state_changed), t);
        g_signal_connect(l->data, "icon-changed", G_CALLBACK(on_window_icon_changed), t);
        add_window(t, l->data, FALSE);
    }
    g_signal_connect(t->screen, "window-opened", G_CALLBACK(on_window_opened), t);
    g_signal_connect(t->screen, "window-closed", G_CALLBACK(on_window_closed), t);
    g_signal_connect(t->screen, "active-window-changed", G_CALLBACK(on_active_window_changed), t);
    return t;
}

GPtrArray *app_tracker_items(AppTracker *t)
{
    return t->items;
}

gboolean app_tracker_has_window(AppTracker *t, WnckWindow *window)
{
    return g_hash_table_contains(t->window_items, window);
}

/* ---- user actions -------------------------------------------------------- */

gboolean app_tracker_is_active(AppTracker *t, DockItem *item)
{
    WnckWindow *active = wnck_screen_get_active_window(t->screen);
    return active && g_list_find(item->windows, active);
}

GList *app_tracker_stacked_windows(AppTracker *t, DockItem *item)
{
    GList *result = NULL;
    for (GList *l = wnck_screen_get_windows_stacked(t->screen); l; l = l->next)
        if (g_list_find(item->windows, l->data))
            result = g_list_prepend(result, l->data);
    return g_list_reverse(result);
}

void app_tracker_activate_window(AppTracker *t, WnckWindow *window, guint32 timestamp)
{
    WnckWorkspace *ws = wnck_window_get_workspace(window);
    if (ws && ws != wnck_screen_get_active_workspace(t->screen))
        wnck_workspace_activate(ws, timestamp);
    wnck_window_activate_transient(window, timestamp);
}

/* Dock click: launch, raise, cycle or minimize, like the macOS dock. */
void app_tracker_activate(AppTracker *t, DockItem *item, guint32 timestamp)
{
    GList *windows = app_tracker_stacked_windows(t, item);
    if (!windows) {
        app_tracker_launch(t, item, timestamp);
        return;
    }
    if (app_tracker_is_active(t, item)) {
        if (windows->next)
            app_tracker_activate_window(t, windows->data, timestamp); /* cycle to the bottom-most */
        else
            wnck_window_minimize(windows->data);
    } else {
        WnckWindow *top_visible = NULL;
        for (GList *l = windows; l; l = l->next)
            if (!wnck_window_is_minimized(l->data))
                top_visible = l->data;
        if (top_visible) {
            app_tracker_activate_window(t, top_visible, timestamp);
        } else {
            for (GList *l = windows; l; l = l->next)
                wnck_window_unminimize(l->data, timestamp);
            app_tracker_activate_window(t, g_list_last(windows)->data, timestamp);
        }
    }
    g_list_free(windows);
}

void app_tracker_launch(AppTracker *t, DockItem *item, guint32 timestamp)
{
    if (!item->app_info)
        return;
    g_autoptr(GdkAppLaunchContext) ctx = gdk_display_get_app_launch_context(gdk_display_get_default());
    gdk_app_launch_context_set_timestamp(ctx, timestamp);
    g_autoptr(GError) err = NULL;
    if (!g_app_info_launch(G_APP_INFO(item->app_info), NULL, G_APP_LAUNCH_CONTEXT(ctx), &err)) {
        g_warning("cannot launch %s: %s", item->key, err->message);
        return;
    }
    item->bounce_start = g_get_monotonic_time();
    notify(t);
}

void app_tracker_close_all(AppTracker *t, DockItem *item, guint32 timestamp)
{
    GList *windows = g_list_copy(item->windows); /* closing may modify the list */
    for (GList *l = windows; l; l = l->next)
        wnck_window_close(l->data, timestamp);
    g_list_free(windows);
}

void app_tracker_set_pinned(AppTracker *t, DockItem *item, gboolean pinned)
{
    if (item->pinned == pinned || !item->app_info)
        return;
    g_ptr_array_remove(t->items, item);
    item->pinned = pinned;

    guint n_pinned = 0;
    while (n_pinned < t->items->len && ((DockItem *)g_ptr_array_index(t->items, n_pinned))->pinned)
        n_pinned++;
    if (pinned || item->windows) {
        /* pinned: end of the pinned group; unpinned: start of the running group */
        g_ptr_array_insert(t->items, n_pinned, item);
    } else {
        t->cb.item_removed(item, t->cb.data);
        dock_item_free(item);
    }

    GPtrArray *ids = g_ptr_array_new();
    for (guint i = 0; i < t->items->len; i++) {
        DockItem *it = g_ptr_array_index(t->items, i);
        if (it->pinned)
            g_ptr_array_add(ids, g_strdup(it->key));
    }
    g_ptr_array_add(ids, NULL);
    dock_config_set_pinned(t->cfg, (char **)g_ptr_array_free(ids, FALSE));
    notify(t);
}
