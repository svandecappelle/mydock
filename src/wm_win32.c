/* Window tracking on Windows: WinEvent hooks and the taskbar's rules. */

#include "wm.h"

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#include "win32_util.h"

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif
#ifndef EVENT_OBJECT_CLOAKED
#define EVENT_OBJECT_CLOAKED 0x8017
#define EVENT_OBJECT_UNCLOAKED 0x8018
#endif

#define ICON_SIZE 256
#define GETICON_TIMEOUT_MS 100

struct WmWindow {
    HWND hwnd;
    DWORD pid;
    char *title;     /* last value returned by wm_window_get_title */
    char *app_id;    /* AppUserModelID, or NULL */
    char *app_group; /* executable name without extension, or NULL */
    GdkPixbuf *icon;
    gboolean icon_loaded;
};

struct Wm {
    WmCallbacks cb;
    GHashTable *windows; /* HWND -> WmWindow*: the tracked windows */
    GList *order;        /* WmWindow*, in opening order */
};

/* WinEvent callbacks carry no user data; there is a single Wm per process. */
static Wm *the_wm;

/* ---- which windows belong in the dock ------------------------------------ */

/* Cloaked windows are hidden by DWM though "visible": suspended Store apps,
 * and windows on other virtual desktops, which the taskbar hides too. */
static gboolean is_cloaked(HWND hwnd)
{
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked;
}

/* The taskbar's rules: visible top-level windows that are not tool windows
 * and have no owner, unless they ask for a taskbar button. */
static gboolean belongs_in_tasklist(HWND hwnd)
{
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd)
        return FALSE;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (!(ex & WS_EX_APPWINDOW)) {
        if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))
            return FALSE;
        if (GetWindow(hwnd, GW_OWNER))
            return FALSE;
    }
    return !is_cloaked(hwnd);
}

/* ---- window data --------------------------------------------------------- */

/* The window's own AppUserModelID (set by apps that group their windows,
 * and by the frame windows of Store apps), else the process's if packaged. */
static char *window_aumid(HWND hwnd, DWORD pid)
{
    char *id = NULL;
    IPropertyStore *store = NULL;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, &macdock_IID_IPropertyStore, (void **)&store))) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(IPropertyStore_GetValue(store, &macdock_PKEY_AppUserModel_ID, &value))
            && value.vt == VT_LPWSTR && value.pwszVal && *value.pwszVal)
            id = win32_utf8(value.pwszVal);
        PropVariantClear(&value);
        IPropertyStore_Release(store);
    }
    return id ? id : win32_process_aumid(pid);
}

static char *process_group(DWORD pid)
{
    g_autofree char *path = win32_process_path(pid);
    if (!path)
        return NULL;
    char *name = g_path_get_basename(path);
    char *dot = strrchr(name, '.');
    if (dot && g_ascii_strcasecmp(dot, ".exe") == 0)
        *dot = '\0';
    return name;
}

static WmWindow *window_new(HWND hwnd)
{
    WmWindow *w = g_new0(WmWindow, 1);
    w->hwnd = hwnd;
    GetWindowThreadProcessId(hwnd, &w->pid);
    w->app_id = window_aumid(hwnd, w->pid);
    w->app_group = process_group(w->pid);
    return w;
}

static void window_free(WmWindow *w)
{
    g_free(w->title);
    g_free(w->app_id);
    g_free(w->app_group);
    g_clear_object(&w->icon);
    g_free(w);
}

/* ---- tracking ------------------------------------------------------------ */

static void track(Wm *wm, HWND hwnd, gboolean notify)
{
    WmWindow *w = window_new(hwnd);
    g_hash_table_insert(wm->windows, hwnd, w);
    wm->order = g_list_append(wm->order, w);
    if (notify)
        wm->cb.opened(w, wm->cb.data);
}

static void forget(Wm *wm, HWND hwnd)
{
    WmWindow *w = g_hash_table_lookup(wm->windows, hwnd);
    if (!w)
        return;
    g_hash_table_remove(wm->windows, hwnd);
    wm->order = g_list_remove(wm->order, w);
    wm->cb.closed(w, wm->cb.data);
    window_free(w);
}

/* Start or stop tracking the window if it entered or left the task list. */
static void update(Wm *wm, HWND hwnd)
{
    gboolean tracked = g_hash_table_contains(wm->windows, hwnd);
    gboolean belongs = belongs_in_tasklist(hwnd);
    if (belongs && !tracked)
        track(wm, hwnd, TRUE);
    else if (!belongs && tracked)
        forget(wm, hwnd);
}

/* Titles are read when needed; a title change is the cue to check whether
 * the app set its AppUserModelID after creating the window. */
static void on_name_change(Wm *wm, HWND hwnd)
{
    WmWindow *w = g_hash_table_lookup(wm->windows, hwnd);
    if (!w)
        return;
    char *id = window_aumid(hwnd, w->pid);
    if (g_strcmp0(id, w->app_id) == 0) {
        g_free(id);
        return;
    }
    g_free(w->app_id);
    w->app_id = id;
    wm->cb.app_changed(w, wm->cb.data);
}

static void CALLBACK on_win_event(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG id_object, LONG id_child,
                                  DWORD thread, DWORD time)
{
    Wm *wm = the_wm;
    if (!wm || !hwnd)
        return;
    if (event == EVENT_SYSTEM_FOREGROUND) {
        update(wm, hwnd); /* a window may become eligible as it is activated */
        wm->cb.active_changed(wm->cb.data);
        return;
    }
    if (id_object != OBJID_WINDOW || id_child != CHILDID_SELF)
        return;
    switch (event) {
    case EVENT_OBJECT_DESTROY:
        forget(wm, hwnd);
        break;
    case EVENT_OBJECT_NAMECHANGE:
        on_name_change(wm, hwnd);
        break;
    default: /* shown, hidden, cloaked, uncloaked */
        if (g_hash_table_contains(wm->windows, hwnd) || GetAncestor(hwnd, GA_ROOT) == hwnd)
            update(wm, hwnd);
        break;
    }
}

static BOOL CALLBACK collect_window(HWND hwnd, LPARAM data)
{
    GPtrArray *hwnds = (GPtrArray *)data;
    g_ptr_array_add(hwnds, hwnd);
    return TRUE;
}

/* All top-level windows, top-most first. */
static GPtrArray *top_level_windows(void)
{
    GPtrArray *hwnds = g_ptr_array_new();
    EnumWindows(collect_window, (LPARAM)hwnds);
    return hwnds;
}

Wm *wm_new(const WmCallbacks *callbacks)
{
    g_return_val_if_fail(the_wm == NULL, the_wm);
    win32_com_init();
    Wm *wm = g_new0(Wm, 1);
    wm->cb = *callbacks;
    wm->windows = g_hash_table_new(g_direct_hash, g_direct_equal);
    the_wm = wm;

    /* Opening order is unknown for existing windows: use bottom-most first. */
    GPtrArray *hwnds = top_level_windows();
    for (guint i = hwnds->len; i-- > 0;) {
        HWND hwnd = g_ptr_array_index(hwnds, i);
        if (belongs_in_tasklist(hwnd))
            track(wm, hwnd, FALSE);
    }
    g_ptr_array_free(hwnds, TRUE);

    /* Out-of-context hooks are called from this thread's message loop,
     * which GDK runs. */
    DWORD flags = WINEVENT_OUTOFCONTEXT;
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, on_win_event, 0, 0, flags);
    SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, NULL, on_win_event, 0, 0, flags);
    SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, NULL, on_win_event, 0, 0, flags);
    SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, NULL, on_win_event, 0, 0, flags);
    return wm;
}

GList *wm_get_windows(Wm *wm)
{
    return g_list_copy(wm->order);
}

GList *wm_get_windows_stacked(Wm *wm)
{
    GList *result = NULL;
    GPtrArray *hwnds = top_level_windows();
    for (guint i = 0; i < hwnds->len; i++) { /* prepending reverses to bottom-most first */
        WmWindow *w = g_hash_table_lookup(wm->windows, g_ptr_array_index(hwnds, i));
        if (w)
            result = g_list_prepend(result, w);
    }
    g_ptr_array_free(hwnds, TRUE);
    return result;
}

WmWindow *wm_get_active_window(Wm *wm)
{
    /* The foreground window may be a dialog owned by a tracked window. */
    for (HWND hwnd = GetForegroundWindow(); hwnd; hwnd = GetWindow(hwnd, GW_OWNER)) {
        WmWindow *w = g_hash_table_lookup(wm->windows, hwnd);
        if (w)
            return w;
    }
    return NULL;
}

/* ---- window properties --------------------------------------------------- */

const char *wm_window_get_title(WmWindow *window)
{
    int len = GetWindowTextLengthW(window->hwnd);
    wchar_t *text = g_new0(wchar_t, len + 1);
    GetWindowTextW(window->hwnd, text, len + 1);
    g_free(window->title);
    window->title = win32_utf8(text);
    g_free(text);
    if (!window->title)
        window->title = g_strdup("");
    return window->title;
}

const char *wm_window_get_app_id(WmWindow *window)
{
    return window->app_id;
}

const char *wm_window_get_app_group(WmWindow *window)
{
    return window->app_group;
}

int wm_window_get_pid(WmWindow *window)
{
    return (int)window->pid;
}

/* The icon the window shows in its title bar (usually 32 px). */
static GdkPixbuf *window_class_icon(HWND hwnd)
{
    DWORD_PTR icon = 0;
    if (!SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, GETICON_TIMEOUT_MS, &icon)
        || !icon)
        icon = GetClassLongPtrW(hwnd, GCLP_HICON);
    if (!icon)
        return NULL;
    ICONINFO info;
    if (!GetIconInfo((HICON)icon, &info))
        return NULL;
    GdkPixbuf *pixbuf = info.hbmColor ? win32_bitmap_to_pixbuf(info.hbmColor) : NULL;
    if (info.hbmColor)
        DeleteObject(info.hbmColor);
    if (info.hbmMask)
        DeleteObject(info.hbmMask);
    return pixbuf;
}

GdkPixbuf *wm_window_get_icon(WmWindow *window)
{
    if (!window->icon_loaded) {
        window->icon_loaded = TRUE;
        /* The executable's icon is sharp at dock sizes; the window's icon is
         * a fallback (hosts like ApplicationFrameHost have a generic one). */
        g_autofree char *path = win32_process_path(window->pid);
        window->icon = path ? win32_shell_icon(path, ICON_SIZE) : NULL;
        if (!window->icon)
            window->icon = window_class_icon(window->hwnd);
    }
    return window->icon;
}

gboolean wm_window_is_minimized(WmWindow *window)
{
    return IsIconic(window->hwnd);
}

/* ---- actions ------------------------------------------------------------- */

void wm_window_activate(Wm *wm, WmWindow *window, guint32 timestamp)
{
    HWND hwnd = window->hwnd;
    if (IsIconic(hwnd))
        ShowWindowAsync(hwnd, SW_RESTORE);
    /* Like the taskbar, bring up the window's open dialog if it has one;
     * Windows switches virtual desktop by itself if needed. */
    HWND target = GetLastActivePopup(hwnd);
    if (!target || !IsWindowVisible(target))
        target = hwnd;
    if (SetForegroundWindow(target))
        return;
    /* Windows only lets the process that received the last input take the
     * foreground. A synthetic Alt key press makes this process that one. */
    INPUT alt[2] = { 0 };
    alt[0].type = alt[1].type = INPUT_KEYBOARD;
    alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
    alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &alt[0], sizeof(INPUT));
    SetForegroundWindow(target);
    SendInput(1, &alt[1], sizeof(INPUT));
}

void wm_window_minimize(WmWindow *window)
{
    ShowWindowAsync(window->hwnd, SW_MINIMIZE);
}

void wm_window_unminimize(WmWindow *window, guint32 timestamp)
{
    ShowWindowAsync(window->hwnd, SW_RESTORE);
}

void wm_window_close(WmWindow *window, guint32 timestamp)
{
    PostMessageW(window->hwnd, WM_CLOSE, 0, 0);
}
