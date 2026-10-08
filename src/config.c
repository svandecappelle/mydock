#include "config.h"

#include <errno.h>

#include "appinfo.h"
#include "themes.h"

#define GROUP "Dock"

static const char *const position_names[] = { "bottom", "top", "left", "right" };

const char *dock_position_to_string(DockPosition position)
{
    return position_names[CLAMP((int)position, 0, (int)G_N_ELEMENTS(position_names) - 1)];
}

DockPosition dock_position_from_string(const char *name)
{
    for (guint i = 0; i < G_N_ELEMENTS(position_names); i++)
        if (g_strcmp0(name, position_names[i]) == 0)
            return (DockPosition)i;
    return DOCK_BOTTOM;
}

static const char *const alignment_names[] = { "center", "start", "end" };

const char *dock_alignment_to_string(DockAlignment alignment)
{
    return alignment_names[CLAMP((int)alignment, 0, (int)G_N_ELEMENTS(alignment_names) - 1)];
}

DockAlignment dock_alignment_from_string(const char *name)
{
    for (guint i = 0; i < G_N_ELEMENTS(alignment_names); i++)
        if (g_strcmp0(name, alignment_names[i]) == 0)
            return (DockAlignment)i;
    return ALIGN_CENTER;
}

static gboolean app_installed(const char *id)
{
    g_autoptr(GAppInfo) info = app_info_lookup(id);
    return info != NULL;
}

static void read_int(GKeyFile *kf, const char *key, int *out)
{
    g_autoptr(GError) err = NULL;
    int v = g_key_file_get_integer(kf, GROUP, key, &err);
    if (!err)
        *out = v;
}

static void read_double(GKeyFile *kf, const char *key, double *out)
{
    g_autoptr(GError) err = NULL;
    double v = g_key_file_get_double(kf, GROUP, key, &err);
    if (!err)
        *out = v;
}

static void read_bool(GKeyFile *kf, const char *key, gboolean *out)
{
    g_autoptr(GError) err = NULL;
    gboolean v = g_key_file_get_boolean(kf, GROUP, key, &err);
    if (!err)
        *out = v;
}

void dock_config_reset(DockConfig *cfg)
{
    cfg->position = DOCK_BOTTOM;
    cfg->alignment = ALIGN_CENTER;
    cfg->icon_size = 48;
    cfg->max_scale = 1.8;
    cfg->magnify_range = 3.0;
    cfg->spacing = 6;
    cfg->padding = 8;
    cfg->margin = 6;
    cfg->corner_radius = 16;
    g_free(cfg->theme);
    cfg->theme = g_strdup(THEME_DEFAULT);
    cfg->monitor = -1;
    cfg->reserve_space = TRUE;
    cfg->autohide = FALSE;
    cfg->expand = FALSE;
    cfg->show_labels = TRUE;
}

DockConfig *dock_config_load(void)
{
    DockConfig *cfg = g_new0(DockConfig, 1);
    dock_config_reset(cfg);
    cfg->path = g_build_filename(g_get_user_config_dir(), "macdock", "config.ini", NULL);

    g_autoptr(GKeyFile) kf = g_key_file_new();
    g_autoptr(GError) err = NULL;
    if (!g_key_file_load_from_file(kf, cfg->path, G_KEY_FILE_NONE, &err)
        && !g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_warning("ignoring invalid %s: %s", cfg->path, err->message);

    read_int(kf, "icon_size", &cfg->icon_size);
    read_double(kf, "max_scale", &cfg->max_scale);
    read_double(kf, "magnify_range", &cfg->magnify_range);
    read_int(kf, "spacing", &cfg->spacing);
    read_int(kf, "padding", &cfg->padding);
    read_int(kf, "margin", &cfg->margin);
    read_int(kf, "corner_radius", &cfg->corner_radius);
    read_int(kf, "monitor", &cfg->monitor);
    read_bool(kf, "reserve_space", &cfg->reserve_space);
    read_bool(kf, "autohide", &cfg->autohide);
    read_bool(kf, "expand", &cfg->expand);
    read_bool(kf, "show_labels", &cfg->show_labels);
    g_autofree char *position = g_key_file_get_string(kf, GROUP, "position", NULL);
    if (position)
        cfg->position = dock_position_from_string(position);
    g_autofree char *alignment = g_key_file_get_string(kf, GROUP, "alignment", NULL);
    if (alignment)
        cfg->alignment = dock_alignment_from_string(alignment);
    char *theme = g_key_file_get_string(kf, GROUP, "theme", NULL);
    if (theme) {
        g_free(cfg->theme);
        cfg->theme = theme;
    }
    cfg->icon_size = CLAMP(cfg->icon_size, 16, 256);
    cfg->max_scale = CLAMP(cfg->max_scale, 1.0, 4.0);
    cfg->magnify_range = MAX(cfg->magnify_range, 0.5);

    if (g_key_file_has_key(kf, GROUP, "pinned", NULL)) {
        cfg->pinned = g_key_file_get_string_list(kf, GROUP, "pinned", NULL, NULL);
        if (!cfg->pinned)
            cfg->pinned = g_new0(char *, 1);
    } else {
        GPtrArray *ids = g_ptr_array_new();
        for (const char *const *id = app_info_default_pinned(); *id; id++)
            if (app_installed(*id))
                g_ptr_array_add(ids, g_strdup(*id));
        g_ptr_array_add(ids, NULL);
        cfg->pinned = (char **)g_ptr_array_free(ids, FALSE);
        dock_config_save(cfg);
    }
    return cfg;
}

void dock_config_save(const DockConfig *cfg)
{
    g_autoptr(GKeyFile) kf = g_key_file_new();
    g_key_file_set_comment(kf, NULL, NULL, " macdock configuration - restart the dock after editing", NULL);
    g_key_file_set_string(kf, GROUP, "position", dock_position_to_string(cfg->position));
    g_key_file_set_string(kf, GROUP, "alignment", dock_alignment_to_string(cfg->alignment));
    g_key_file_set_integer(kf, GROUP, "icon_size", cfg->icon_size);
    g_key_file_set_double(kf, GROUP, "max_scale", cfg->max_scale);
    g_key_file_set_double(kf, GROUP, "magnify_range", cfg->magnify_range);
    g_key_file_set_integer(kf, GROUP, "spacing", cfg->spacing);
    g_key_file_set_integer(kf, GROUP, "padding", cfg->padding);
    g_key_file_set_integer(kf, GROUP, "margin", cfg->margin);
    g_key_file_set_integer(kf, GROUP, "corner_radius", cfg->corner_radius);
    g_key_file_set_string(kf, GROUP, "theme", cfg->theme);
    g_key_file_set_integer(kf, GROUP, "monitor", cfg->monitor);
    g_key_file_set_boolean(kf, GROUP, "reserve_space", cfg->reserve_space);
    g_key_file_set_boolean(kf, GROUP, "autohide", cfg->autohide);
    g_key_file_set_boolean(kf, GROUP, "expand", cfg->expand);
    g_key_file_set_boolean(kf, GROUP, "show_labels", cfg->show_labels);
    g_key_file_set_string_list(kf, GROUP, "pinned", (const char *const *)cfg->pinned,
                               g_strv_length(cfg->pinned));

    g_autofree char *dir = g_path_get_dirname(cfg->path);
    g_autoptr(GError) err = NULL;
    if (g_mkdir_with_parents(dir, 0755) != 0 || !g_key_file_save_to_file(kf, cfg->path, &err))
        g_warning("cannot save %s: %s", cfg->path, err ? err->message : g_strerror(errno));
}

void dock_config_set_pinned(DockConfig *cfg, char **ids)
{
    g_strfreev(cfg->pinned);
    cfg->pinned = ids;
    dock_config_save(cfg);
}
