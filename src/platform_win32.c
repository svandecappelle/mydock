/* Platform services on Windows. */

#include "platform.h"

#include <windows.h>
#include <dwmapi.h>
#include <gdk/gdkwin32.h>
#include <shellapi.h>
#include <stdlib.h>

#define APPBAR_MESSAGE (WM_APP + 0x51)

static HWND window_handle(GtkWidget *window)
{
    GdkWindow *gdk_window = gtk_widget_get_window(window);
    return gdk_window ? gdk_win32_window_get_handle(gdk_window) : NULL;
}

/* No taskbar button, no focus on click, above other windows, and left alone
 * by window animations and Aero Peek. */
static void apply_dock_styles(GtkWidget *window)
{
    HWND hwnd = window_handle(window);
    if (!hwnd)
        return;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, (ex | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) & ~WS_EX_APPWINDOW);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    BOOL on = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &on, sizeof on);
    DwmSetWindowAttribute(hwnd, DWMWA_EXCLUDED_FROM_PEEK, &on, sizeof on);
}

void platform_setup_dock_window(GtkWidget *window)
{
    apply_dock_styles(window);
    /* GDK recomputes the extended style when it shows the window: apply
     * ours again after that. */
    if (!g_object_get_data(G_OBJECT(window), "macdock-styles")) {
        g_object_set_data(G_OBJECT(window), "macdock-styles", GINT_TO_POINTER(1));
        g_signal_connect_after(window, "map", G_CALLBACK(apply_dock_styles), NULL);
    }
}

/* GTK draws RGBA undecorated windows as layered windows with per-pixel
 * alpha, and Windows lets clicks on fully transparent pixels through. GDK's
 * input shape only redirects clicks to windows of this process. */
gboolean platform_input_follows_alpha(void)
{
    return TRUE;
}

/* ---- reserved space: the AppBar API -------------------------------------- */

static struct {
    HWND hwnd;        /* registered appbar window, or NULL */
    UINT edge;        /* ABE_* */
    RECT rect;        /* the reserved strip, in screen pixels */
    gboolean cleanup_installed;
} appbar;

static void appbar_remove(void)
{
    if (!appbar.hwnd)
        return;
    APPBARDATA abd = { .cbSize = sizeof abd, .hWnd = appbar.hwnd };
    SHAppBarMessage(ABM_REMOVE, &abd);
    appbar.hwnd = NULL;
}

/* Ask the shell for the strip; it may push it inwards, past other appbars
 * such as the taskbar on the same edge. Keep its thickness. */
static void appbar_set_pos(void)
{
    APPBARDATA abd = { .cbSize = sizeof abd, .hWnd = appbar.hwnd, .uEdge = appbar.edge, .rc = appbar.rect };
    SHAppBarMessage(ABM_QUERYPOS, &abd);
    switch (appbar.edge) {
    case ABE_TOP:    abd.rc.bottom = abd.rc.top + (appbar.rect.bottom - appbar.rect.top); break;
    case ABE_LEFT:   abd.rc.right = abd.rc.left + (appbar.rect.right - appbar.rect.left); break;
    case ABE_RIGHT:  abd.rc.left = abd.rc.right - (appbar.rect.right - appbar.rect.left); break;
    default:         abd.rc.top = abd.rc.bottom - (appbar.rect.bottom - appbar.rect.top); break;
    }
    SHAppBarMessage(ABM_SETPOS, &abd);
}

/* Notifications the shell sends to registered appbars. */
static GdkFilterReturn appbar_filter(GdkXEvent *xevent, GdkEvent *event, gpointer data)
{
    MSG *msg = (MSG *)xevent;
    if (msg->message != APPBAR_MESSAGE || !appbar.hwnd)
        return GDK_FILTER_CONTINUE;
    switch (msg->wParam) {
    case ABN_POSCHANGED: /* another appbar or the taskbar moved */
        appbar_set_pos();
        break;
    case ABN_FULLSCREENAPP: /* stay below full-screen apps such as videos */
        SetWindowPos(appbar.hwnd, msg->lParam ? HWND_BOTTOM : HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        break;
    }
    return GDK_FILTER_REMOVE;
}

/* The shell keeps the space reserved until told otherwise. */
static void on_unrealize(GtkWidget *window, gpointer data)
{
    appbar_remove();
}

static void on_exit(void)
{
    appbar_remove();
}

void platform_reserve_space(GtkWidget *window, DockPosition position, const GdkRectangle *monitor, int thickness)
{
    HWND hwnd = window_handle(window);
    if (!hwnd)
        return;
    if (thickness <= 0) {
        appbar_remove();
        return;
    }
    if (appbar.hwnd != hwnd) {
        appbar_remove();
        APPBARDATA abd = { .cbSize = sizeof abd, .hWnd = hwnd, .uCallbackMessage = APPBAR_MESSAGE };
        if (!SHAppBarMessage(ABM_NEW, &abd))
            return;
        appbar.hwnd = hwnd;
        gdk_window_add_filter(gtk_widget_get_window(window), appbar_filter, NULL);
        if (!appbar.cleanup_installed) {
            appbar.cleanup_installed = TRUE;
            g_signal_connect(window, "unrealize", G_CALLBACK(on_unrealize), NULL);
            atexit(on_exit);
        }
    }

    /* Work in screen pixels, on the monitor the dock window was moved to:
     * GDK's coordinates are scaled and offset from Windows' ones. */
    MONITORINFO info = { .cbSize = sizeof info };
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &info);
    RECT r = info.rcMonitor;
    LONG thick = thickness * gtk_widget_get_scale_factor(window);
    switch (position) {
    case DOCK_TOP:   appbar.edge = ABE_TOP, r.bottom = r.top + thick; break;
    case DOCK_LEFT:  appbar.edge = ABE_LEFT, r.right = r.left + thick; break;
    case DOCK_RIGHT: appbar.edge = ABE_RIGHT, r.left = r.right - thick; break;
    default:         appbar.edge = ABE_BOTTOM, r.top = r.bottom - thick; break;
    }
    appbar.rect = r;
    appbar_set_pos();
}

/* Only used without desktop composition, which Windows 8 and later always
 * have. The window region clips both drawing and clicks. */
void platform_set_window_shape(GtkWidget *window, const cairo_region_t *region)
{
    HWND hwnd = window_handle(window);
    if (!hwnd)
        return;
    int sf = gtk_widget_get_scale_factor(window);
    HRGN shape = CreateRectRgn(0, 0, 0, 0);
    for (int i = 0; i < cairo_region_num_rectangles(region); i++) {
        cairo_rectangle_int_t r;
        cairo_region_get_rectangle(region, i, &r);
        HRGN part = CreateRectRgn(r.x * sf, r.y * sf, (r.x + r.width) * sf, (r.y + r.height) * sf);
        CombineRgn(shape, shape, part, RGN_OR);
        DeleteObject(part);
    }
    SetWindowRgn(hwnd, shape, TRUE); /* the system owns the region now */
}

/* ---- quitting ------------------------------------------------------------ */

static GSourceFunc quit_func;
static gpointer quit_data;

/* Runs on a system thread: hand over to the main loop. */
static BOOL WINAPI on_console_event(DWORD type)
{
    if (!quit_func)
        return FALSE;
    g_idle_add(quit_func, quit_data);
    return TRUE;
}

/* Ctrl+C or closing the console, when started from one. */
void platform_on_quit_request(GSourceFunc func, gpointer data)
{
    quit_func = func;
    quit_data = data;
    SetConsoleCtrlHandler(on_console_event, TRUE);
}
