#pragma once

#include <gio/gio.h>

#include "wm.h"

/* Installed applications, implemented per platform (appinfo_desktop.c reads
 * freedesktop .desktop files). Apps are identified by a string id, the one
 * stored in the config's pinned list. */

/* The app with this id, or NULL if it isn't installed. Free with g_object_unref. */
GAppInfo *app_info_lookup(const char *id);
/* Apps the user may pick from, unsorted. Free with g_list_free_full(…, g_object_unref). */
GList *app_info_list(void);
/* Ids to pin on first run, if installed; NULL-terminated. */
const char *const *app_info_default_pinned(void);

/* Finds which installed app a window belongs to. Follows apps being
 * installed and removed. */
typedef struct AppIndex AppIndex;

AppIndex *app_index_new(void);
/* The window's app, or NULL if none matches (owned by the index). */
GAppInfo *app_index_match(AppIndex *index, WmWindow *window);
