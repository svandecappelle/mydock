#pragma once

#include <gtk/gtk.h>

#include "config.h"
#include "wm.h"

/* One icon in the dock: an application and its open windows. */
typedef struct {
    char *key;                 /* desktop id, or "wmclass:<name>" for unknown apps */
    GAppInfo *app_info;        /* NULL for windows of no known installed app */
    gboolean pinned;
    GList *windows;            /* WmWindow*, in opening order */
    gint64 bounce_start;       /* monotonic µs of a launch animation, or 0 */
    GHashTable *icons;         /* size -> GdkPixbuf */
} DockItem;

const char *dock_item_name(DockItem *item);
/* Pixbuf rendered at `size` px; owned by the item (cached). */
GdkPixbuf *dock_item_icon(DockItem *item, int size);
void dock_item_clear_icons(DockItem *item);

typedef struct {
    void (*changed)(gpointer data);                     /* the item list changed */
    void (*item_removed)(DockItem *item, gpointer data); /* called just before freeing */
    gpointer data;
} AppTrackerCallbacks;

/* Keeps the ordered list of DockItems in sync with the config and the
 * windows open on screen. */
typedef struct AppTracker AppTracker;

AppTracker *app_tracker_new(DockConfig *cfg, const AppTrackerCallbacks *callbacks);
GPtrArray *app_tracker_items(AppTracker *t); /* DockItem*, pinned first */
gboolean app_tracker_is_active(AppTracker *t, DockItem *item);
gboolean app_tracker_has_window(AppTracker *t, WmWindow *window);
GList *app_tracker_stacked_windows(AppTracker *t, DockItem *item); /* bottom-most first; free with g_list_free */
void app_tracker_activate(AppTracker *t, DockItem *item, guint32 timestamp);
void app_tracker_activate_window(AppTracker *t, WmWindow *window, guint32 timestamp);
void app_tracker_launch(AppTracker *t, DockItem *item, guint32 timestamp);
void app_tracker_close_all(AppTracker *t, DockItem *item, guint32 timestamp);
void app_tracker_set_pinned(AppTracker *t, DockItem *item, gboolean pinned);
/* Pins exactly `ids` (NULL-terminated), in that order, and saves the config. */
void app_tracker_set_pinned_ids(AppTracker *t, const char *const *ids);
/* Moves `item` to position `index` of the pinned group (`to_pinned`) or of the
 * group of other running apps. Dropping an unpinned app among the pinned ones
 * pins it; apps not known to be installed can't be pinned. Saves the config. */
void app_tracker_move_item(AppTracker *t, DockItem *item, guint index, gboolean to_pinned);
