#pragma once

#include <glib.h>

/* Colour themes for the dock. A config's `theme` is a theme id, or
 * THEME_AUTO to follow the desktop's dark or light preference. */

#define THEME_AUTO "auto"
#define THEME_DEFAULT "dark"

typedef double Rgba[4];

typedef struct {
    const char *id;   /* stored in the config */
    const char *name; /* shown in the settings */
    Rgba bar_top, bar_bottom; /* bar gradient */
    Rgba border, highlight, shadow;
    Rgba separator, indicator;
    Rgba label_bg, label_fg;
} Theme;

guint theme_count(void);
const Theme *theme_get(guint index);
/* The theme with this id, or NULL (THEME_AUTO is not a theme). */
const Theme *theme_lookup(const char *id);
/* The theme to draw with for `id`: THEME_AUTO picks the default dark or
 * light theme after `dark_desktop`; unknown ids give THEME_DEFAULT. */
const Theme *theme_resolve(const char *id, gboolean dark_desktop);
