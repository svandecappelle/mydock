#pragma once

#include <glib.h>

/* User configuration, stored in ~/.config/macdock/config.ini. */
typedef struct {
    int icon_size;          /* icon size at rest, in px */
    double max_scale;       /* magnification of the icon under the pointer */
    double magnify_range;   /* how many neighbouring icons the zoom spreads over */
    int spacing;            /* gap between icons */
    int padding;            /* inner padding of the bar */
    int margin;             /* gap between bar and screen edge */
    int corner_radius;
    char *theme;            /* "dark" or "light" */
    int monitor;            /* -1 = primary monitor */
    gboolean reserve_space; /* keep maximized windows above the dock */
    gboolean show_labels;
    char **pinned;          /* NULL-terminated list of desktop ids */
    char *path;
} DockConfig;

DockConfig *dock_config_load(void);
void dock_config_save(const DockConfig *cfg);
/* Replaces the pinned list (takes ownership of `ids`) and saves. */
void dock_config_set_pinned(DockConfig *cfg, char **ids);
