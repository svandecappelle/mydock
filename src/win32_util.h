#pragma once

/* Helpers shared by the Windows backends. */

#include <windows.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <propsys.h>
#include <shobjidl.h>

/* Defined here rather than taken from libuuid, which lacks some of them
 * depending on the MinGW version. */
extern const IID macdock_IID_IPropertyStore;
extern const IID macdock_IID_IShellItem;
extern const IID macdock_IID_IShellItem2;
extern const IID macdock_IID_IShellItemImageFactory;
extern const IID macdock_IID_IEnumShellItems;
extern const GUID macdock_BHID_EnumItems;
extern const PROPERTYKEY macdock_PKEY_AppUserModel_ID;
extern const PROPERTYKEY macdock_PKEY_Link_TargetParsingPath;

/* Initializes COM (single-threaded apartment) on the calling thread, once. */
void win32_com_init(void);

/* UTF-16 <-> UTF-8; free with g_free. NULL in, NULL out. */
char *win32_utf8(const wchar_t *text);
wchar_t *win32_utf16(const char *text);

/* Full path of the executable of process `pid`, or NULL. Free with g_free. */
char *win32_process_path(DWORD pid);
/* AppUserModelID of a packaged (Store) process, or NULL. Free with g_free. */
char *win32_process_aumid(DWORD pid);

/* A 32-bit (A)RGB bitmap as a pixbuf, or NULL. Does not free `bitmap`. */
GdkPixbuf *win32_bitmap_to_pixbuf(HBITMAP bitmap);
/* The shell's icon for `parsing_name` (a file path, or "shell:AppsFolder\…"),
 * at least `size` px if available, or NULL. */
GdkPixbuf *win32_shell_icon(const char *parsing_name, int size);
