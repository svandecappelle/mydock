#pragma once

#include <gtk/gtk.h>

#include "config.h"

/* The dock window: a transparent, full-width DOCK window at the bottom of
 * the monitor, painted entirely with Cairo. */
typedef struct Dock Dock;

Dock *dock_new(GtkApplication *app, DockConfig *cfg);
GtkWidget *dock_get_window(Dock *dock);
/* Opens (or raises) the settings window. */
void dock_show_preferences(Dock *dock, guint32 timestamp);
