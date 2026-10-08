/* Installed applications on Windows: the Start menu's "All apps" list
 * (shell:AppsFolder), which covers both desktop programs and Store apps.
 * An app's id is its parsing name in that folder: its AppUserModelID, or a
 * known-folder relative path for programs without one. */

#include "appinfo.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string.h>

#include "win32_util.h"

#define ICON_SIZE 256
#define RESCAN_INTERVAL_US (30 * G_USEC_PER_SEC) /* rescan for an unknown window at most this often */
#define N_TIERS 3

static const char *const default_pinned[] = {
    "Microsoft.Windows.Explorer",
    "MSEdge",
    "Chrome",
    "308046B0AF4A39CB", /* Firefox */
    "Microsoft.WindowsTerminal_8wekyb3d8bbwe!App",
    "Microsoft.WindowsNotepad_8wekyb3d8bbwe!App",
    "windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel",
    NULL,
};

static gboolean is_exe(const char *path)
{
    size_t len = strlen(path);
    return len >= 4 && g_ascii_strcasecmp(path + len - 4, ".exe") == 0;
}

/* ---- MacdockShellApp: a GAppInfo for an AppsFolder entry ----------------- */

#define MACDOCK_TYPE_SHELL_APP (macdock_shell_app_get_type())
G_DECLARE_FINAL_TYPE(MacdockShellApp, macdock_shell_app, MACDOCK, SHELL_APP, GObject)

struct _MacdockShellApp {
    GObject parent;
    char *id;   /* parsing name in shell:AppsFolder */
    char *name;
    char *exe;  /* target executable, or NULL (Store apps, or unknown) */
    GIcon *icon;
    gboolean icon_loaded;
};

static void macdock_shell_app_iface_init(GAppInfoIface *iface);

G_DEFINE_TYPE_WITH_CODE(MacdockShellApp, macdock_shell_app, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_APP_INFO, macdock_shell_app_iface_init))

static void macdock_shell_app_finalize(GObject *object)
{
    MacdockShellApp *app = MACDOCK_SHELL_APP(object);
    g_free(app->id);
    g_free(app->name);
    g_free(app->exe);
    g_clear_object(&app->icon);
    G_OBJECT_CLASS(macdock_shell_app_parent_class)->finalize(object);
}

static void macdock_shell_app_class_init(MacdockShellAppClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = macdock_shell_app_finalize;
}

static void macdock_shell_app_init(MacdockShellApp *app)
{
}

static MacdockShellApp *shell_app_new(const char *id, const char *name, const char *exe)
{
    MacdockShellApp *app = g_object_new(MACDOCK_TYPE_SHELL_APP, NULL);
    app->id = g_strdup(id);
    app->name = g_strdup(name);
    app->exe = g_strdup(exe);
    return app;
}

static GAppInfo *shell_app_dup(GAppInfo *info)
{
    MacdockShellApp *app = MACDOCK_SHELL_APP(info);
    MacdockShellApp *copy = shell_app_new(app->id, app->name, app->exe);
    copy->icon = app->icon ? g_object_ref(app->icon) : NULL;
    copy->icon_loaded = app->icon_loaded;
    return G_APP_INFO(copy);
}

static gboolean shell_app_equal(GAppInfo *a, GAppInfo *b)
{
    return g_ascii_strcasecmp(MACDOCK_SHELL_APP(a)->id, MACDOCK_SHELL_APP(b)->id) == 0;
}

static const char *shell_app_get_id(GAppInfo *info)
{
    return MACDOCK_SHELL_APP(info)->id;
}

static const char *shell_app_get_name(GAppInfo *info)
{
    return MACDOCK_SHELL_APP(info)->name;
}

static const char *shell_app_get_description(GAppInfo *info)
{
    return NULL;
}

static const char *shell_app_get_executable(GAppInfo *info)
{
    MacdockShellApp *app = MACDOCK_SHELL_APP(info);
    return app->exe ? app->exe : "";
}

static const char *shell_app_get_commandline(GAppInfo *info)
{
    return shell_app_get_executable(info);
}

static GIcon *shell_app_get_icon(GAppInfo *info)
{
    MacdockShellApp *app = MACDOCK_SHELL_APP(info);
    if (!app->icon_loaded) {
        app->icon_loaded = TRUE;
        g_autofree char *path = g_strconcat("shell:AppsFolder\\", app->id, NULL);
        GdkPixbuf *pixbuf = win32_shell_icon(path, ICON_SIZE);
        app->icon = pixbuf ? G_ICON(pixbuf) : NULL; /* GdkPixbuf implements GIcon */
    }
    return app->icon;
}

/* The shell starts desktop programs and Store apps alike from their
 * AppsFolder entry. */
static gboolean shell_app_launch(GAppInfo *info, GList *files, GAppLaunchContext *context, GError **error)
{
    MacdockShellApp *app = MACDOCK_SHELL_APP(info);
    g_autofree char *target = g_strconcat("shell:AppsFolder\\", app->id, NULL);
    g_autofree wchar_t *wtarget = win32_utf16(target);
    SHELLEXECUTEINFOW exec = { 0 };
    exec.cbSize = sizeof exec;
    exec.fMask = SEE_MASK_FLAG_NO_UI;
    exec.lpVerb = L"open";
    exec.lpFile = wtarget;
    exec.nShow = SW_SHOWNORMAL;
    AllowSetForegroundWindow(ASFW_ANY); /* let the app take the focus */
    if (!ShellExecuteExW(&exec)) {
        DWORD code = GetLastError();
        g_autofree char *message = g_win32_error_message(code);
        g_set_error(error, G_IO_ERROR, g_io_error_from_win32_error(code), "%s", message);
        return FALSE;
    }
    return TRUE;
}

static gboolean shell_app_launch_uris(GAppInfo *info, GList *uris, GAppLaunchContext *context, GError **error)
{
    return shell_app_launch(info, NULL, context, error);
}

static gboolean shell_app_supports_nothing(GAppInfo *info)
{
    return FALSE;
}

/* Hide uninstallers, help files and web links: the entries that don't start
 * a program. */
static gboolean shell_app_should_show(GAppInfo *info)
{
    MacdockShellApp *app = MACDOCK_SHELL_APP(info);
    if (app->exe && !is_exe(app->exe))
        return FALSE;
    g_autofree char *exe_name = app->exe ? g_path_get_basename(app->exe) : NULL;
    if (exe_name && g_ascii_strncasecmp(exe_name, "unins", 5) == 0)
        return FALSE;
    g_autofree char *name = g_utf8_casefold(app->name, -1);
    return !strstr(name, "uninstall");
}

static void macdock_shell_app_iface_init(GAppInfoIface *iface)
{
    iface->dup = shell_app_dup;
    iface->equal = shell_app_equal;
    iface->get_id = shell_app_get_id;
    iface->get_name = shell_app_get_name;
    iface->get_description = shell_app_get_description;
    iface->get_executable = shell_app_get_executable;
    iface->get_commandline = shell_app_get_commandline;
    iface->get_icon = shell_app_get_icon;
    iface->launch = shell_app_launch;
    iface->launch_uris = shell_app_launch_uris;
    iface->supports_uris = shell_app_supports_nothing;
    iface->supports_files = shell_app_supports_nothing;
    iface->should_show = shell_app_should_show;
}

/* ---- the catalogue ------------------------------------------------------- */

static GHashTable *catalog;   /* lowercase id -> MacdockShellApp* */
static gint64 catalog_time;   /* monotonic µs of the last scan */
static guint catalog_generation;

static char *item_string(IShellItem *item, SIGDN kind)
{
    wchar_t *text = NULL;
    if (FAILED(IShellItem_GetDisplayName(item, kind, &text)))
        return NULL;
    char *result = win32_utf8(text);
    CoTaskMemFree(text);
    return result;
}

/* "{known folder GUID}\rest" -> the full path. */
static char *resolve_known_folder(const char *path)
{
    const char *close = path[0] == '{' ? strchr(path, '}') : NULL;
    if (!close || close[1] != '\\')
        return NULL;
    g_autofree char *guid_text = g_strndup(path, close + 1 - path);
    g_autofree wchar_t *wguid = win32_utf16(guid_text);
    GUID folder_id;
    wchar_t *folder = NULL;
    if (FAILED(CLSIDFromString(wguid, &folder_id)) || FAILED(SHGetKnownFolderPath(&folder_id, 0, NULL, &folder)))
        return NULL;
    g_autofree char *base = win32_utf8(folder);
    CoTaskMemFree(folder);
    return g_build_filename(base, close + 2, NULL);
}

static char *item_target(IShellItem *item, const char *id)
{
    char *target = NULL;
    IShellItem2 *item2 = NULL;
    if (SUCCEEDED(IShellItem_QueryInterface(item, &macdock_IID_IShellItem2, (void **)&item2))) {
        wchar_t *text = NULL;
        if (SUCCEEDED(IShellItem2_GetString(item2, &macdock_PKEY_Link_TargetParsingPath, &text))) {
            target = win32_utf8(text);
            CoTaskMemFree(text);
        }
        IShellItem2_Release(item2);
    }
    if (!target && strchr(id, '\\'))
        target = id[0] == '{' ? resolve_known_folder(id) : g_strdup(id);
    return target;
}

static void scan_catalog(void)
{
    win32_com_init();
    if (catalog)
        g_hash_table_remove_all(catalog);
    else
        catalog = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    catalog_time = g_get_monotonic_time();
    catalog_generation++;

    IShellItem *folder = NULL;
    IEnumShellItems *items = NULL;
    if (FAILED(SHCreateItemFromParsingName(L"shell:AppsFolder", NULL, &macdock_IID_IShellItem, (void **)&folder)))
        return;
    if (SUCCEEDED(IShellItem_BindToHandler(folder, NULL, &macdock_BHID_EnumItems, &macdock_IID_IEnumShellItems,
                                           (void **)&items))) {
        IShellItem *item;
        while (IEnumShellItems_Next(items, 1, &item, NULL) == S_OK) {
            g_autofree char *id = item_string(item, SIGDN_PARENTRELATIVEPARSING);
            g_autofree char *name = item_string(item, SIGDN_NORMALDISPLAY);
            if (id && name) {
                g_autofree char *exe = item_target(item, id);
                char *key = g_ascii_strdown(id, -1);
                if (g_hash_table_contains(catalog, key))
                    g_free(key);
                else
                    g_hash_table_insert(catalog, key, shell_app_new(id, name, exe));
            }
            IShellItem_Release(item);
        }
        IEnumShellItems_Release(items);
    }
    IShellItem_Release(folder);
}

static void ensure_catalog(void)
{
    if (!catalog)
        scan_catalog();
}

/* Apps installed since the last scan are picked up when first needed. */
static gboolean rescan_if_stale(void)
{
    if (g_get_monotonic_time() - catalog_time < RESCAN_INTERVAL_US)
        return FALSE;
    scan_catalog();
    return TRUE;
}

GAppInfo *app_info_lookup(const char *id)
{
    ensure_catalog();
    g_autofree char *key = g_ascii_strdown(id, -1);
    MacdockShellApp *app = g_hash_table_lookup(catalog, key);
    if (!app && rescan_if_stale())
        app = g_hash_table_lookup(catalog, key);
    return app ? g_object_ref(G_APP_INFO(app)) : NULL;
}

GList *app_info_list(void)
{
    scan_catalog(); /* the settings window asks: be up to date */
    GList *result = NULL;
    GHashTableIter iter;
    gpointer app;
    g_hash_table_iter_init(&iter, catalog);
    while (g_hash_table_iter_next(&iter, NULL, &app))
        if (g_app_info_should_show(app))
            result = g_list_prepend(result, g_object_ref(app));
    return result;
}

const char *const *app_info_default_pinned(void)
{
    return default_pinned;
}

/* ---- window -> application matching -------------------------------------- */

struct AppIndex {
    guint generation; /* catalog_generation the tiers were built from */
    /* Lowercase name -> MacdockShellApp*, in decreasing reliability:
     * AppUserModelID, executable path, executable name. */
    GHashTable *tiers[N_TIERS];
};

static void index_add(GHashTable *table, const char *name, MacdockShellApp *app)
{
    char *key = g_ascii_strdown(name, -1);
    if (g_hash_table_contains(table, key))
        g_free(key);
    else
        g_hash_table_insert(table, key, g_object_ref(app));
}

static char *exe_basename(const char *path)
{
    char *name = g_path_get_basename(path);
    char *dot = strrchr(name, '.');
    if (dot && g_ascii_strcasecmp(dot, ".exe") == 0)
        *dot = '\0';
    return name;
}

static void build_index(AppIndex *index)
{
    ensure_catalog();
    index->generation = catalog_generation;
    for (int i = 0; i < N_TIERS; i++)
        g_hash_table_remove_all(index->tiers[i]);
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, catalog);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        MacdockShellApp *app = value;
        index_add(index->tiers[0], app->id, app);
        if (app->exe && is_exe(app->exe)) {
            index_add(index->tiers[1], app->exe, app);
            g_autofree char *name = exe_basename(app->exe);
            index_add(index->tiers[2], name, app);
        }
    }
}

AppIndex *app_index_new(void)
{
    AppIndex *index = g_new0(AppIndex, 1);
    for (int i = 0; i < N_TIERS; i++)
        index->tiers[i] = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    build_index(index);
    return index;
}

static GAppInfo *lookup_window(AppIndex *index, WmWindow *window)
{
    g_autofree char *path = win32_process_path((DWORD)wm_window_get_pid(window));
    const char *names[N_TIERS] = { wm_window_get_app_id(window), path, wm_window_get_app_group(window) };
    for (int tier = 0; tier < N_TIERS; tier++) {
        if (!names[tier] || !*names[tier])
            continue;
        g_autofree char *key = g_ascii_strdown(names[tier], -1);
        MacdockShellApp *app = g_hash_table_lookup(index->tiers[tier], key);
        if (app)
            return G_APP_INFO(app);
    }
    return NULL;
}

GAppInfo *app_index_match(AppIndex *index, WmWindow *window)
{
    if (index->generation != catalog_generation)
        build_index(index);
    GAppInfo *app = lookup_window(index, window);
    if (!app && rescan_if_stale()) {
        build_index(index);
        app = lookup_window(index, window);
    }
    return app;
}
