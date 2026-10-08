#include "apps.h"

#include "appinfo.h"

struct AppTracker {
    DockConfig *cfg;
    AppTrackerCallbacks cb;
    Wm *wm;
    AppIndex *apps;
    GPtrArray *items;            /* DockItem* (owned, freed by hand) */
    GHashTable *window_items;    /* WmWindow* -> DockItem* */
};

/* ---- DockItem ------------------------------------------------------------ */

static DockItem *dock_item_new(const char *key, GAppInfo *info, gboolean pinned)
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
        return g_app_info_get_display_name(item->app_info);
    if (item->windows) {
        WmWindow *w = item->windows->data;
        const char *name = wm_window_get_app_group(w);
        return name && *name ? name : wm_window_get_title(w);
    }
    return item->key;
}

static GdkPixbuf *load_icon(DockItem *item, int size)
{
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    GIcon *gicon = item->app_info ? g_app_info_get_icon(item->app_info) : NULL;
    if (gicon) {
        GtkIconInfo *info = gtk_icon_theme_lookup_by_gicon(theme, gicon, size, GTK_ICON_LOOKUP_FORCE_SIZE);
        if (info) {
            GdkPixbuf *pixbuf = gtk_icon_info_load_icon(info, NULL);
            g_object_unref(info);
            if (pixbuf)
                return pixbuf;
        }
    }
    for (GList *l = item->windows; l; l = l->next) {
        GdkPixbuf *icon = wm_window_get_icon(l->data);
        if (icon)
            return g_object_ref(icon);
    }

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

/* ---- window bookkeeping -------------------------------------------------- */

static char *key_for(WmWindow *w, GAppInfo *info)
{
    if (info)
        return g_strdup(g_app_info_get_id(info));
    const char *group = wm_window_get_app_group(w);
    if (group && *group) {
        g_autofree char *lower = g_ascii_strdown(group, -1);
        return g_strconcat("wmclass:", lower, NULL);
    }
    return g_strdup_printf("wmclass:pid-%d", wm_window_get_pid(w));
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

static void add_window(AppTracker *t, WmWindow *w, gboolean do_notify)
{
    if (g_hash_table_contains(t->window_items, w))
        return;
    GAppInfo *info = app_index_match(t->apps, w);
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

static void remove_window(AppTracker *t, WmWindow *w, gboolean do_notify)
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

/* Re-file a window that now claims to belong to another app. */
static void on_window_app_changed(WmWindow *w, gpointer data)
{
    AppTracker *t = data;
    DockItem *current = g_hash_table_lookup(t->window_items, w);
    g_autofree char *key = key_for(w, app_index_match(t->apps, w));
    if (current && g_str_equal(current->key, key))
        return;
    remove_window(t, w, FALSE);
    add_window(t, w, FALSE);
    notify(t);
}

static void on_window_icon_changed(WmWindow *w, gpointer data)
{
    AppTracker *t = data;
    DockItem *item = g_hash_table_lookup(t->window_items, w);
    if (item && !item->app_info) {
        dock_item_clear_icons(item);
        notify(t);
    }
}

static void on_window_opened(WmWindow *w, gpointer data)
{
    add_window(data, w, TRUE);
}

static void on_window_closed(WmWindow *w, gpointer data)
{
    remove_window(data, w, TRUE);
}

static void on_active_window_changed(gpointer data)
{
    notify(data);
}

AppTracker *app_tracker_new(DockConfig *cfg, const AppTrackerCallbacks *callbacks)
{
    AppTracker *t = g_new0(AppTracker, 1);
    t->cfg = cfg;
    t->cb = *callbacks;
    t->items = g_ptr_array_new();
    t->window_items = g_hash_table_new(g_direct_hash, g_direct_equal);
    t->apps = app_index_new();

    for (char **id = cfg->pinned; *id; id++) {
        g_autoptr(GAppInfo) info = app_info_lookup(*id);
        if (info && !find_item(t, *id))
            g_ptr_array_add(t->items, dock_item_new(*id, info, TRUE));
    }

    WmCallbacks wm_callbacks = {
        on_window_opened, on_window_closed, on_window_app_changed, on_window_icon_changed,
        on_active_window_changed, t,
    };
    t->wm = wm_new(&wm_callbacks);
    GList *windows = wm_get_windows(t->wm);
    for (GList *l = windows; l; l = l->next)
        add_window(t, l->data, FALSE);
    g_list_free(windows);
    return t;
}

GPtrArray *app_tracker_items(AppTracker *t)
{
    return t->items;
}

gboolean app_tracker_has_window(AppTracker *t, WmWindow *window)
{
    return g_hash_table_contains(t->window_items, window);
}

/* ---- user actions -------------------------------------------------------- */

gboolean app_tracker_is_active(AppTracker *t, DockItem *item)
{
    WmWindow *active = wm_get_active_window(t->wm);
    return active && g_list_find(item->windows, active);
}

GList *app_tracker_stacked_windows(AppTracker *t, DockItem *item)
{
    GList *stacked = wm_get_windows_stacked(t->wm);
    GList *result = NULL;
    for (GList *l = stacked; l; l = l->next)
        if (g_list_find(item->windows, l->data))
            result = g_list_prepend(result, l->data);
    g_list_free(stacked);
    return g_list_reverse(result);
}

void app_tracker_activate_window(AppTracker *t, WmWindow *window, guint32 timestamp)
{
    wm_window_activate(t->wm, window, timestamp);
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
            wm_window_minimize(windows->data);
    } else {
        WmWindow *top_visible = NULL;
        for (GList *l = windows; l; l = l->next)
            if (!wm_window_is_minimized(l->data))
                top_visible = l->data;
        if (top_visible) {
            app_tracker_activate_window(t, top_visible, timestamp);
        } else {
            for (GList *l = windows; l; l = l->next)
                wm_window_unminimize(l->data, timestamp);
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
    if (!g_app_info_launch(item->app_info, NULL, G_APP_LAUNCH_CONTEXT(ctx), &err)) {
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
        wm_window_close(l->data, timestamp);
    g_list_free(windows);
}

void app_tracker_set_pinned_ids(AppTracker *t, const char *const *ids)
{
    GPtrArray *items = g_ptr_array_new();
    GPtrArray *pinned = g_ptr_array_new();

    /* Pinned items first, in the requested order. */
    for (const char *const *id = ids; *id; id++) {
        DockItem *item = find_item(t, *id);
        if (item) {
            if (!item->app_info || g_ptr_array_find(items, item, NULL))
                continue;
            g_ptr_array_remove(t->items, item);
        } else {
            g_autoptr(GAppInfo) info = app_info_lookup(*id);
            if (!info)
                continue;
            item = dock_item_new(*id, info, TRUE);
        }
        item->pinned = TRUE;
        g_ptr_array_add(items, item);
        g_ptr_array_add(pinned, g_strdup(*id));
    }

    /* Then the remaining items that still have windows, in their current
     * order; a newly unpinned app thus lands at the start of that group. */
    for (guint i = 0; i < t->items->len; i++) {
        DockItem *item = g_ptr_array_index(t->items, i);
        item->pinned = FALSE;
        if (item->windows) {
            g_ptr_array_add(items, item);
        } else {
            t->cb.item_removed(item, t->cb.data);
            dock_item_free(item);
        }
    }
    g_ptr_array_free(t->items, TRUE);
    t->items = items;

    g_ptr_array_add(pinned, NULL);
    dock_config_set_pinned(t->cfg, (char **)g_ptr_array_free(pinned, FALSE));
    notify(t);
}

void app_tracker_move_item(AppTracker *t, DockItem *item, guint index, gboolean to_pinned)
{
    if (!g_ptr_array_remove(t->items, item))
        return;
    if (!item->app_info)
        to_pinned = FALSE;
    item->pinned = to_pinned;

    guint n_pinned = 0;
    while (n_pinned < t->items->len && ((DockItem *)g_ptr_array_index(t->items, n_pinned))->pinned)
        n_pinned++;
    guint pos = to_pinned ? MIN(index, n_pinned) : n_pinned + MIN(index, t->items->len - n_pinned);
    g_ptr_array_insert(t->items, pos, item);

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

void app_tracker_set_pinned(AppTracker *t, DockItem *item, gboolean pinned)
{
    if (item->pinned == pinned || !item->app_info)
        return;
    GPtrArray *ids = g_ptr_array_new();
    for (guint i = 0; i < t->items->len; i++) {
        DockItem *it = g_ptr_array_index(t->items, i);
        if (it->pinned && it != item)
            g_ptr_array_add(ids, it->key);
    }
    if (pinned)
        g_ptr_array_add(ids, item->key);
    g_ptr_array_add(ids, NULL);
    app_tracker_set_pinned_ids(t, (const char *const *)ids->pdata);
    g_ptr_array_free(ids, TRUE);
}
