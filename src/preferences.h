#pragma once

#include <gtk/gtk.h>

#include "apps.h"
#include "config.h"

/* The "Dock Settings" window. Every change is applied to the running dock
 * immediately (through `apply`) and saved to the config file. */
typedef struct Preferences Preferences;

typedef struct {
    void (*apply)(gpointer data);  /* config fields changed: re-layout the dock */
    void (*closed)(gpointer data); /* the window was destroyed; the Preferences is freed */
    gpointer data;
} PreferencesCallbacks;

Preferences *preferences_new(GtkApplication *app, DockConfig *cfg, AppTracker *tracker,
                             const PreferencesCallbacks *callbacks);
void preferences_present(Preferences *prefs, guint32 timestamp);
/* Call when the pinned list may have changed outside the window. */
void preferences_sync(Preferences *prefs);
