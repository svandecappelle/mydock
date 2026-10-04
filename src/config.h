#pragma once

#include <glib.h>

/* The size padding, spacing and corner radius are expressed at: they all
 * scale with icon_size, so the dock keeps its proportions at any size. */
#define DOCK_BASE_SIZE 48

typedef enum {
    DOCK_BOTTOM,
    DOCK_TOP,
    DOCK_LEFT,
    DOCK_RIGHT,
} DockPosition;

/* User configuration, stored in ~/.config/macdock/config.ini. */
typedef struct {
    DockPosition position;  /* screen edge the dock is attached to */
    int icon_size;          /* icon size at rest, in px: the overall dock size */
    double max_scale;       /* magnification of the icon under the pointer */
    double magnify_range;   /* how many neighbouring icons the zoom spreads over */
    int spacing;            /* gap between icons, at DOCK_BASE_SIZE */
    int padding;            /* inner padding of the bar, at DOCK_BASE_SIZE */
    int margin;             /* gap between bar and screen edge (not scaled) */
    int corner_radius;      /* at DOCK_BASE_SIZE */
    char *theme;            /* "dark" or "light" */
    int monitor;            /* -1 = primary monitor */
    gboolean reserve_space; /* keep maximized windows above the dock */
    gboolean autohide;      /* slide out of view until the pointer touches the screen edge */
    gboolean expand;        /* stretch the bar along the whole screen edge, like a panel */
    gboolean show_labels;
    char **pinned;          /* NULL-terminated list of desktop ids */
    char *path;
} DockConfig;

DockConfig *dock_config_load(void);
/* Restores every setting except `pinned` to its default (does not save). */
void dock_config_reset(DockConfig *cfg);
void dock_config_save(const DockConfig *cfg);
/* Replaces the pinned list (takes ownership of `ids`) and saves. */
void dock_config_set_pinned(DockConfig *cfg, char **ids);

const char *dock_position_to_string(DockPosition position);
DockPosition dock_position_from_string(const char *name); /* DOCK_BOTTOM if unknown */
