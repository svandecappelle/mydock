/* Installed applications on freedesktop systems: .desktop files. */

#include "appinfo.h"

#include <gio/gdesktopappinfo.h>
#include <string.h>

#define N_TIERS 3

static const char *const default_pinned[] = {
    "pcmanfm-qt.desktop",
    "org.gnome.Nautilus.desktop",
    "qterminal.desktop",
    "firefox.desktop",
    "featherpad.desktop",
    "vlc.desktop",
    "lxqt-config.desktop",
    NULL,
};

GAppInfo *app_info_lookup(const char *id)
{
    GDesktopAppInfo *info = g_desktop_app_info_new(id);
    return info ? G_APP_INFO(info) : NULL;
}

GList *app_info_list(void)
{
    GList *result = NULL;
    GList *all = g_app_info_get_all();
    for (GList *l = all; l; l = l->next)
        if (G_IS_DESKTOP_APP_INFO(l->data) && g_app_info_should_show(l->data))
            result = g_list_prepend(result, g_object_ref(l->data));
    g_list_free_full(all, g_object_unref);
    return g_list_reverse(result);
}

const char *const *app_info_default_pinned(void)
{
    return default_pinned;
}

/* ---- window -> application matching -------------------------------------- */

struct AppIndex {
    /* Lowercase name -> GDesktopAppInfo, in decreasing reliability:
     * StartupWMClass, desktop id (and its last dotted part), executable name. */
    GHashTable *tiers[N_TIERS];
};

static void index_add(GHashTable *table, const char *name, GDesktopAppInfo *info)
{
    char *key = g_ascii_strdown(name, -1);
    if (g_hash_table_contains(table, key))
        g_free(key);
    else
        g_hash_table_insert(table, key, g_object_ref(info));
}

static void build_index(AppIndex *index)
{
    for (int i = 0; i < N_TIERS; i++)
        g_hash_table_remove_all(index->tiers[i]);

    GList *all = g_app_info_get_all();
    for (GList *l = all; l; l = l->next) {
        if (!G_IS_DESKTOP_APP_INFO(l->data))
            continue;
        GDesktopAppInfo *info = l->data;

        const char *wm_class = g_desktop_app_info_get_startup_wm_class(info);
        if (wm_class)
            index_add(index->tiers[0], wm_class, info);

        const char *id = g_app_info_get_id(G_APP_INFO(info));
        if (id) {
            g_autofree char *base = g_str_has_suffix(id, ".desktop")
                ? g_strndup(id, strlen(id) - strlen(".desktop")) : g_strdup(id);
            index_add(index->tiers[1], base, info);
            const char *dot = strrchr(base, '.');
            if (dot)
                index_add(index->tiers[1], dot + 1, info);
        }

        const char *exe = g_app_info_get_executable(G_APP_INFO(info));
        if (exe) {
            g_autofree char *exe_name = g_path_get_basename(exe);
            index_add(index->tiers[2], exe_name, info);
        }
    }
    g_list_free_full(all, g_object_unref);
}

static void on_apps_changed(GAppInfoMonitor *monitor, AppIndex *index)
{
    build_index(index);
}

AppIndex *app_index_new(void)
{
    AppIndex *index = g_new0(AppIndex, 1);
    for (int i = 0; i < N_TIERS; i++)
        index->tiers[i] = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    build_index(index);
    g_signal_connect(g_app_info_monitor_get(), "changed", G_CALLBACK(on_apps_changed), index);
    return index;
}

GAppInfo *app_index_match(AppIndex *index, WmWindow *window)
{
    const char *names[] = { wm_window_get_app_id(window), wm_window_get_app_group(window) };
    for (int tier = 0; tier < N_TIERS; tier++) {
        for (guint n = 0; n < G_N_ELEMENTS(names); n++) {
            if (!names[n] || !*names[n])
                continue;
            g_autofree char *key = g_ascii_strdown(names[n], -1);
            GDesktopAppInfo *info = g_hash_table_lookup(index->tiers[tier], key);
            if (info)
                return G_APP_INFO(info);
        }
    }
    return NULL;
}
