#include "themes.h"

/* Colours are RGBA in 0..1. The bar is translucent when a compositor runs;
 * without one it is drawn with the same colours, opaque. */
static const Theme themes[] = {
    {
        .id = "dark", .name = "Dark",
        .bar_top = { 0.24, 0.24, 0.26, 0.55 },
        .bar_bottom = { 0.13, 0.13, 0.15, 0.65 },
        .border = { 1, 1, 1, 0.18 },
        .highlight = { 1, 1, 1, 0.12 },
        .shadow = { 0, 0, 0, 0.05 },
        .separator = { 1, 1, 1, 0.28 },
        .indicator = { 1, 1, 1, 0.85 },
        .label_bg = { 0.14, 0.14, 0.16, 0.88 },
        .label_fg = { 1, 1, 1, 0.95 },
    },
    {
        .id = "light", .name = "Light",
        .bar_top = { 1, 1, 1, 0.55 },
        .bar_bottom = { 0.90, 0.90, 0.93, 0.62 },
        .border = { 1, 1, 1, 0.55 },
        .highlight = { 1, 1, 1, 0.45 },
        .shadow = { 0, 0, 0, 0.04 },
        .separator = { 0, 0, 0, 0.22 },
        .indicator = { 0.1, 0.1, 0.1, 0.8 },
        .label_bg = { 0.96, 0.96, 0.97, 0.92 },
        .label_fg = { 0.08, 0.08, 0.08, 0.95 },
    },
    {
        /* Neutral mid grey, a little more solid than Dark. */
        .id = "graphite", .name = "Graphite",
        .bar_top = { 0.38, 0.38, 0.40, 0.66 },
        .bar_bottom = { 0.25, 0.25, 0.27, 0.74 },
        .border = { 1, 1, 1, 0.22 },
        .highlight = { 1, 1, 1, 0.14 },
        .shadow = { 0, 0, 0, 0.05 },
        .separator = { 1, 1, 1, 0.32 },
        .indicator = { 1, 1, 1, 0.9 },
        .label_bg = { 0.22, 0.22, 0.24, 0.94 },
        .label_fg = { 1, 1, 1, 0.97 },
    },
    {
        /* Barely there: a frosted outline over the wallpaper. */
        .id = "clear", .name = "Clear Glass",
        .bar_top = { 1, 1, 1, 0.20 },
        .bar_bottom = { 1, 1, 1, 0.10 },
        .border = { 1, 1, 1, 0.40 },
        .highlight = { 1, 1, 1, 0.22 },
        .shadow = { 0, 0, 0, 0.03 },
        .separator = { 1, 1, 1, 0.45 },
        .indicator = { 1, 1, 1, 0.95 },
        .label_bg = { 0, 0, 0, 0.72 },
        .label_fg = { 1, 1, 1, 1 },
    },
    {
        .id = "midnight", .name = "Midnight",
        .bar_top = { 0.10, 0.14, 0.28, 0.66 },
        .bar_bottom = { 0.04, 0.06, 0.16, 0.76 },
        .border = { 0.55, 0.70, 1, 0.26 },
        .highlight = { 0.60, 0.75, 1, 0.12 },
        .shadow = { 0, 0, 0.05, 0.06 },
        .separator = { 0.60, 0.75, 1, 0.32 },
        .indicator = { 0.45, 0.70, 1, 0.95 },
        .label_bg = { 0.06, 0.09, 0.20, 0.93 },
        .label_fg = { 0.88, 0.93, 1, 0.97 },
    },
    {
        /* Nord palette: polar night, snow storm, frost. */
        .id = "nord", .name = "Nord",
        .bar_top = { 0.231, 0.259, 0.322, 0.72 },     /* #3B4252 */
        .bar_bottom = { 0.180, 0.204, 0.251, 0.80 },  /* #2E3440 */
        .border = { 0.925, 0.937, 0.957, 0.16 },      /* #ECEFF4 */
        .highlight = { 0.925, 0.937, 0.957, 0.08 },
        .shadow = { 0, 0, 0, 0.05 },
        .separator = { 0.298, 0.337, 0.416, 0.95 },   /* #4C566A */
        .indicator = { 0.533, 0.753, 0.816, 1 },      /* #88C0D0 */
        .label_bg = { 0.180, 0.204, 0.251, 0.96 },
        .label_fg = { 0.925, 0.937, 0.957, 1 },
    },
    {
        /* Dracula palette. */
        .id = "dracula", .name = "Dracula",
        .bar_top = { 0.267, 0.278, 0.353, 0.72 },     /* #44475A */
        .bar_bottom = { 0.157, 0.165, 0.212, 0.82 },  /* #282A36 */
        .border = { 0.741, 0.576, 0.976, 0.32 },      /* #BD93F9 */
        .highlight = { 0.973, 0.973, 0.949, 0.08 },
        .shadow = { 0, 0, 0, 0.06 },
        .separator = { 0.384, 0.447, 0.643, 0.95 },   /* #6272A4 */
        .indicator = { 1, 0.475, 0.776, 1 },          /* #FF79C6 */
        .label_bg = { 0.157, 0.165, 0.212, 0.96 },
        .label_fg = { 0.973, 0.973, 0.949, 1 },       /* #F8F8F2 */
    },
    {
        /* Solarized dark. */
        .id = "solarized", .name = "Solarized",
        .bar_top = { 0.027, 0.212, 0.259, 0.74 },     /* base02 */
        .bar_bottom = { 0, 0.169, 0.212, 0.82 },      /* base03 */
        .border = { 0.576, 0.631, 0.631, 0.24 },      /* base1 */
        .highlight = { 0.933, 0.910, 0.835, 0.07 },
        .shadow = { 0, 0, 0, 0.05 },
        .separator = { 0.345, 0.431, 0.459, 0.95 },   /* base01 */
        .indicator = { 0.710, 0.537, 0, 1 },          /* yellow */
        .label_bg = { 0, 0.169, 0.212, 0.96 },
        .label_fg = { 0.933, 0.910, 0.835, 1 },       /* base2 */
    },
    {
        /* Warm light pink. */
        .id = "rose", .name = "Rosé",
        .bar_top = { 1, 0.94, 0.95, 0.60 },
        .bar_bottom = { 0.97, 0.84, 0.87, 0.68 },
        .border = { 1, 1, 1, 0.55 },
        .highlight = { 1, 1, 1, 0.45 },
        .shadow = { 0.30, 0, 0.10, 0.04 },
        .separator = { 0.55, 0.20, 0.32, 0.28 },
        .indicator = { 0.80, 0.25, 0.45, 0.92 },
        .label_bg = { 1, 0.95, 0.96, 0.95 },
        .label_fg = { 0.35, 0.08, 0.18, 0.97 },
    },
    {
        /* Opaque black and white with a yellow running dot, for legibility. */
        .id = "high-contrast", .name = "High Contrast",
        .bar_top = { 0, 0, 0, 0.94 },
        .bar_bottom = { 0, 0, 0, 0.97 },
        .border = { 1, 1, 1, 0.95 },
        .highlight = { 1, 1, 1, 0 },
        .shadow = { 0, 0, 0, 0.08 },
        .separator = { 1, 1, 1, 0.95 },
        .indicator = { 1, 0.85, 0, 1 },
        .label_bg = { 0, 0, 0, 1 },
        .label_fg = { 1, 1, 1, 1 },
    },
};

guint theme_count(void)
{
    return G_N_ELEMENTS(themes);
}

const Theme *theme_get(guint index)
{
    return index < G_N_ELEMENTS(themes) ? &themes[index] : NULL;
}

const Theme *theme_lookup(const char *id)
{
    for (guint i = 0; i < G_N_ELEMENTS(themes); i++)
        if (g_strcmp0(themes[i].id, id) == 0)
            return &themes[i];
    return NULL;
}

const Theme *theme_resolve(const char *id, gboolean dark_desktop)
{
    if (g_strcmp0(id, THEME_AUTO) == 0)
        return theme_lookup(dark_desktop ? "dark" : "light");
    const Theme *theme = theme_lookup(id);
    return theme ? theme : theme_lookup(THEME_DEFAULT);
}
