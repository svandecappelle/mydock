#pragma once

#include <gio/gdesktopappinfo.h>
#include <gtk/gtk.h>
#include <libwnck/libwnck.h>

#include "config.h"

/* One icon in the dock: an application and its open windows. */
typedef struct {
    char *key;                 /* desktop id, or "wmclass:<name>" for unknown apps */
    GDesktopAppInfo *app_info; /* NULL for windows without a .desktop file */
    gboolean pinned;
    GList *windows;            /* WnckWindow*, in opening order (not referenced) */
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
gboolean app_tracker_has_window(AppTracker *t, WnckWindow *window);
GList *app_tracker_stacked_windows(AppTracker *t, DockItem *item); /* bottom-most first; free with g_list_free */
void app_tracker_activate(AppTracker *t, DockItem *item, guint32 timestamp);
void app_tracker_activate_window(AppTracker *t, WnckWindow *window, guint32 timestamp);
void app_tracker_launch(AppTracker *t, DockItem *item, guint32 timestamp);
void app_tracker_close_all(AppTracker *t, DockItem *item, guint32 timestamp);
void app_tracker_set_pinned(AppTracker *t, DockItem *item, gboolean pinned);
/* Pins exactly `ids` (NULL-terminated), in that order, and saves the config. */
void app_tracker_set_pinned_ids(AppTracker *t, const char *const *ids);
