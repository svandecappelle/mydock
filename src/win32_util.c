#include "win32_util.h"

const IID macdock_IID_IPropertyStore =
    { 0x886d8eeb, 0x8cf2, 0x4446, { 0x8d, 0x02, 0xcd, 0xba, 0x1d, 0xbd, 0xcf, 0x99 } };
const IID macdock_IID_IShellItem =
    { 0x43826d1e, 0xe718, 0x42ee, { 0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe } };
const IID macdock_IID_IShellItem2 =
    { 0x7e9fb0d3, 0x919f, 0x4307, { 0xab, 0x2e, 0x9b, 0x18, 0x60, 0x31, 0x0c, 0x93 } };
const IID macdock_IID_IShellItemImageFactory =
    { 0xbcc18b79, 0xba16, 0x442f, { 0x80, 0xc4, 0x8a, 0x59, 0xc3, 0x0c, 0x46, 0x3b } };
const IID macdock_IID_IEnumShellItems =
    { 0x70629033, 0xe363, 0x4a28, { 0xa5, 0x67, 0x0d, 0xb7, 0x80, 0x06, 0xe6, 0xd7 } };
const GUID macdock_BHID_EnumItems =
    { 0x94f60519, 0x2850, 0x4924, { 0xaa, 0x5a, 0xd1, 0x5e, 0x84, 0x86, 0x80, 0x39 } };
const PROPERTYKEY macdock_PKEY_AppUserModel_ID =
    { { 0x9f4c2855, 0x9f79, 0x4b39, { 0xa8, 0xd0, 0xe1, 0xd4, 0x2d, 0xe1, 0xd5, 0xf3 } }, 5 };
const PROPERTYKEY macdock_PKEY_Link_TargetParsingPath =
    { { 0xb9b4b3fc, 0x2b51, 0x4a42, { 0xb5, 0xd8, 0x32, 0x41, 0x46, 0xaf, 0xcf, 0x25 } }, 2 };

void win32_com_init(void)
{
    static gboolean done;
    if (done)
        return;
    done = TRUE;
    /* S_FALSE or RPC_E_CHANGED_MODE: GTK already initialized it, fine too. */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
}

char *win32_utf8(const wchar_t *text)
{
    return text ? g_utf16_to_utf8((const gunichar2 *)text, -1, NULL, NULL, NULL) : NULL;
}

wchar_t *win32_utf16(const char *text)
{
    return text ? (wchar_t *)g_utf8_to_utf16(text, -1, NULL, NULL, NULL) : NULL;
}

char *win32_process_path(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return NULL;
    wchar_t path[MAX_PATH * 2];
    DWORD len = G_N_ELEMENTS(path);
    char *result = QueryFullProcessImageNameW(process, 0, path, &len) ? win32_utf8(path) : NULL;
    CloseHandle(process);
    return result;
}

typedef LONG(WINAPI *GetApplicationUserModelIdFunc)(HANDLE process, UINT32 *len, PWSTR id);

char *win32_process_aumid(DWORD pid)
{
    /* Windows 8+; looked up at run time as not every SDK declares it. */
    static GetApplicationUserModelIdFunc get_aumid;
    static gboolean looked_up;
    if (!looked_up) {
        looked_up = TRUE;
        get_aumid = (GetApplicationUserModelIdFunc)(void (*)(void))GetProcAddress(
            GetModuleHandleW(L"kernel32.dll"), "GetApplicationUserModelId");
    }
    if (!get_aumid)
        return NULL;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return NULL;
    wchar_t id[256]; /* APPLICATION_USER_MODEL_ID_MAX_LENGTH is 130 */
    UINT32 len = G_N_ELEMENTS(id);
    char *result = get_aumid(process, &len, id) == ERROR_SUCCESS ? win32_utf8(id) : NULL;
    CloseHandle(process);
    return result;
}

GdkPixbuf *win32_bitmap_to_pixbuf(HBITMAP bitmap)
{
    BITMAP bm;
    if (!GetObjectW(bitmap, sizeof bm, &bm) || bm.bmWidth <= 0 || bm.bmHeight == 0)
        return NULL;
    int w = bm.bmWidth, h = abs(bm.bmHeight);
    BITMAPINFO info = { 0 };
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h; /* top-down rows */
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    guchar *bgra = g_malloc((gsize)w * h * 4);
    HDC dc = GetDC(NULL);
    int rows = GetDIBits(dc, bitmap, 0, h, bgra, &info, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);
    if (rows != h) {
        g_free(bgra);
        return NULL;
    }

    /* Bitmaps without an alpha channel have it all zero. Shell bitmaps that
     * have one are usually premultiplied: no colour exceeds its alpha. */
    gboolean has_alpha = FALSE, premultiplied = TRUE;
    for (gsize i = 0; i < (gsize)w * h; i++) {
        const guchar *p = bgra + i * 4;
        has_alpha |= p[3] != 0;
        premultiplied &= p[0] <= p[3] && p[1] <= p[3] && p[2] <= p[3];
    }

    GdkPixbuf *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, w, h);
    guchar *pixels = gdk_pixbuf_get_pixels(pixbuf);
    int stride = gdk_pixbuf_get_rowstride(pixbuf);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const guchar *s = bgra + ((gsize)y * w + x) * 4;
            guchar *d = pixels + (gsize)y * stride + x * 4;
            guchar a = has_alpha ? s[3] : 255;
            for (int c = 0; c < 3; c++) {
                guint v = s[2 - c];
                if (has_alpha && premultiplied && a > 0 && a < 255)
                    v = MIN(255, (v * 255 + a / 2) / a);
                d[c] = (guchar)v;
            }
            d[3] = a;
        }
    }
    g_free(bgra);
    return pixbuf;
}

GdkPixbuf *win32_shell_icon(const char *parsing_name, int size)
{
    win32_com_init();
    g_autofree wchar_t *name = win32_utf16(parsing_name);
    IShellItemImageFactory *factory = NULL;
    if (!name || FAILED(SHCreateItemFromParsingName(name, NULL, &macdock_IID_IShellItemImageFactory,
                                                    (void **)&factory)))
        return NULL;
    SIZE wanted = { size, size };
    HBITMAP bitmap = NULL;
    HRESULT hr = IShellItemImageFactory_GetImage(factory, wanted, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bitmap);
    IShellItemImageFactory_Release(factory);
    if (FAILED(hr) || !bitmap)
        return NULL;
    GdkPixbuf *pixbuf = win32_bitmap_to_pixbuf(bitmap);
    DeleteObject(bitmap);
    return pixbuf;
}
