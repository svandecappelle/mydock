#include "preferences.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "appinfo.h"

#define SAVE_DELAY_MS 400

struct Preferences {
    DockConfig *cfg;
    AppTracker *tracker;
    PreferencesCallbacks cb;
    GtkWidget *window;

    GtkWidget *theme, *icon_size, *corner_radius, *show_labels;
    GtkWidget *magnify, *zoom, *spread;
    GtkWidget *position, *alignment, *monitor, *expand, *autohide, *reserve_space, *spacing, *padding, *margin;
    GtkWidget *apps_list, *remove_button, *up_button, *down_button;

    double zoom_value;   /* zoom shown in the slider, kept while magnification is off */
    gboolean updating;   /* widgets are being set from the config: ignore their signals */
    gboolean editing_apps;
    guint save_id;
    char **shown_pinned; /* ids currently listed on the Apps page */
};

/* ---- applying and saving ------------------------------------------------- */

static gboolean save_now(gpointer data)
{
    Preferences *p = data;
    p->save_id = 0;
    dock_config_save(p->cfg);
    return G_SOURCE_REMOVE;
}

/* A config field changed: update the dock now, save a little later so that
 * dragging a slider does not rewrite the file on every step. */
static void changed(Preferences *p)
{
    if (p->updating)
        return;
    p->cb.apply(p->cb.data);
    if (p->save_id)
        g_source_remove(p->save_id);
    p->save_id = g_timeout_add(SAVE_DELAY_MS, save_now, p);
}

static void on_spin_changed(GtkSpinButton *spin, Preferences *p)
{
    *(int *)g_object_get_data(G_OBJECT(spin), "field") = gtk_spin_button_get_value_as_int(spin);
    changed(p);
}

static void on_switch_changed(GtkSwitch *sw, GParamSpec *pspec, Preferences *p)
{
    *(gboolean *)g_object_get_data(G_OBJECT(sw), "field") = gtk_switch_get_active(sw);
    changed(p);
}

static void on_icon_size_changed(GtkRange *range, Preferences *p)
{
    p->cfg->icon_size = (int)round(gtk_range_get_value(range));
    changed(p);
}

static void on_spread_changed(GtkRange *range, Preferences *p)
{
    p->cfg->magnify_range = gtk_range_get_value(range);
    changed(p);
}

static void on_zoom_changed(GtkRange *range, Preferences *p)
{
    p->zoom_value = gtk_range_get_value(range);
    if (gtk_switch_get_active(GTK_SWITCH(p->magnify)))
        p->cfg->max_scale = p->zoom_value;
    changed(p);
}

static void on_magnify_changed(GtkSwitch *sw, GParamSpec *pspec, Preferences *p)
{
    gboolean on = gtk_switch_get_active(sw);
    p->cfg->max_scale = on ? p->zoom_value : 1.0;
    gtk_widget_set_sensitive(p->zoom, on);
    gtk_widget_set_sensitive(p->spread, on);
    changed(p);
}

static void on_autohide_changed(GtkSwitch *sw, GParamSpec *pspec, Preferences *p)
{
    /* an auto-hidden dock never reserves space */
    gtk_widget_set_sensitive(p->reserve_space, !gtk_switch_get_active(sw));
}

static void on_theme_changed(GtkComboBox *combo, Preferences *p)
{
    const char *id = gtk_combo_box_get_active_id(combo);
    if (!id)
        return;
    g_free(p->cfg->theme);
    p->cfg->theme = g_strdup(id);
    changed(p);
}

static void on_position_changed(GtkComboBox *combo, Preferences *p)
{
    const char *id = gtk_combo_box_get_active_id(combo);
    if (!id)
        return;
    p->cfg->position = dock_position_from_string(id);
    changed(p);
}

static void on_alignment_changed(GtkComboBox *combo, Preferences *p)
{
    const char *id = gtk_combo_box_get_active_id(combo);
    if (!id)
        return;
    p->cfg->alignment = dock_alignment_from_string(id);
    changed(p);
}

static void on_monitor_changed(GtkComboBox *combo, Preferences *p)
{
    const char *id = gtk_combo_box_get_active_id(combo);
    if (!id)
        return;
    p->cfg->monitor = atoi(id);
    changed(p);
}

/* Set every widget from the config, without triggering `changed`. */
static void load_values(Preferences *p)
{
    DockConfig *c = p->cfg;
    p->updating = TRUE;
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->theme), c->theme))
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->theme), "dark");
    gtk_range_set_value(GTK_RANGE(p->icon_size), c->icon_size);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->corner_radius), c->corner_radius);
    gtk_switch_set_active(GTK_SWITCH(p->show_labels), c->show_labels);

    gboolean magnify = c->max_scale > 1.0;
    if (magnify)
        p->zoom_value = c->max_scale;
    gtk_range_set_value(GTK_RANGE(p->zoom), p->zoom_value);
    gtk_range_set_value(GTK_RANGE(p->spread), c->magnify_range);
    gtk_switch_set_active(GTK_SWITCH(p->magnify), magnify);
    gtk_widget_set_sensitive(p->zoom, magnify);
    gtk_widget_set_sensitive(p->spread, magnify);

    gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->position), dock_position_to_string(c->position));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->alignment), dock_alignment_to_string(c->alignment));
    g_autofree char *monitor_id = g_strdup_printf("%d", c->monitor);
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->monitor), monitor_id))
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->monitor), "-1");
    gtk_switch_set_active(GTK_SWITCH(p->reserve_space), c->reserve_space);
    gtk_switch_set_active(GTK_SWITCH(p->expand), c->expand);
    gtk_switch_set_active(GTK_SWITCH(p->autohide), c->autohide);
    gtk_widget_set_sensitive(p->reserve_space, !c->autohide);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->spacing), c->spacing);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->padding), c->padding);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->margin), c->margin);
    p->updating = FALSE;
}

static void on_reset(GtkButton *button, Preferences *p)
{
    dock_config_reset(p->cfg);
    p->zoom_value = p->cfg->max_scale;
    load_values(p);
    changed(p);
}

/* ---- widget helpers ------------------------------------------------------ */

static GtkWidget *make_spin(Preferences *p, int min, int max, int *field)
{
    GtkWidget *spin = gtk_spin_button_new_with_range(min, max, 1);
    g_object_set_data(G_OBJECT(spin), "field", field);
    g_signal_connect(spin, "value-changed", G_CALLBACK(on_spin_changed), p);
    return spin;
}

static GtkWidget *make_switch(Preferences *p, gboolean *field)
{
    GtkWidget *sw = gtk_switch_new();
    if (field) {
        g_object_set_data(G_OBJECT(sw), "field", field);
        g_signal_connect(sw, "notify::active", G_CALLBACK(on_switch_changed), p);
    }
    return sw;
}

static GtkWidget *make_scale(double min, double max, double step, int digits, GCallback callback, Preferences *p)
{
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, step);
    gtk_scale_set_digits(GTK_SCALE(scale), digits);
    gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
    gtk_widget_set_size_request(scale, 220, -1);
    g_signal_connect(scale, "value-changed", callback, p);
    return scale;
}

/* A titled group of settings; returns the grid to add rows to. */
static GtkWidget *add_section(GtkWidget *page, const char *title)
{
    GtkWidget *label = gtk_label_new(NULL);
    g_autofree char *markup = g_markup_printf_escaped("<b>%s</b>", title);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(page), label, FALSE, FALSE, 0);

    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
    g_object_set(grid, "margin", 14, NULL);
    gtk_container_add(GTK_CONTAINER(frame), grid);
    gtk_box_pack_start(GTK_BOX(page), frame, FALSE, FALSE, 0);
    return grid;
}

static void add_row(GtkWidget *grid, const char *title, const char *subtitle, GtkWidget *control)
{
    int row = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(grid), "rows"));
    g_object_set_data(G_OBJECT(grid), "rows", GINT_TO_POINTER(row + 1));

    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(text, TRUE);
    gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
    GtkWidget *label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(text), label, FALSE, FALSE, 0);
    if (subtitle) {
        GtkWidget *sub = gtk_label_new(NULL);
        g_autofree char *markup = g_markup_printf_escaped("<small>%s</small>", subtitle);
        gtk_label_set_markup(GTK_LABEL(sub), markup);
        gtk_label_set_xalign(GTK_LABEL(sub), 0);
        gtk_label_set_line_wrap(GTK_LABEL(sub), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(sub), "dim-label");
        gtk_box_pack_start(GTK_BOX(text), sub, FALSE, FALSE, 0);
    }
    gtk_widget_set_halign(control, GTK_ALIGN_END);
    gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(grid), text, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), control, 1, row, 1, 1);
}

static GtkWidget *new_page(void)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    g_object_set(page, "margin", 18, NULL);
    return page;
}

static GtkWidget *scrolled(GtkWidget *child)
{
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), child);
    return sw;
}

/* ---- pages --------------------------------------------------------------- */

static GtkWidget *build_appearance_page(Preferences *p)
{
    GtkWidget *page = new_page();

    GtkWidget *grid = add_section(page, "Appearance");
    p->theme = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->theme), "dark", "Dark");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->theme), "light", "Light");
    g_signal_connect(p->theme, "changed", G_CALLBACK(on_theme_changed), p);
    add_row(grid, "Theme", NULL, p->theme);
    p->icon_size = make_scale(24, 128, 2, 0, G_CALLBACK(on_icon_size_changed), p);
    add_row(grid, "Dock size", "Icon size in pixels; padding, spacing and corners scale with it", p->icon_size);
    p->corner_radius = make_spin(p, 0, 40, &p->cfg->corner_radius);
    add_row(grid, "Corner radius", "Scales with the dock size", p->corner_radius);
    p->show_labels = make_switch(p, &p->cfg->show_labels);
    add_row(grid, "Show app names", "Display the name above the hovered icon", p->show_labels);

    grid = add_section(page, "Magnification");
    p->magnify = make_switch(p, NULL);
    g_signal_connect(p->magnify, "notify::active", G_CALLBACK(on_magnify_changed), p);
    add_row(grid, "Magnify icons", "Enlarge icons under the pointer", p->magnify);
    p->zoom = make_scale(1.1, 3.0, 0.1, 1, G_CALLBACK(on_zoom_changed), p);
    add_row(grid, "Zoom", "Scale of the icon right under the pointer", p->zoom);
    p->spread = make_scale(1.0, 6.0, 0.5, 1, G_CALLBACK(on_spread_changed), p);
    add_row(grid, "Spread", "How many neighbouring icons grow too", p->spread);
    return scrolled(page);
}

static GtkWidget *build_position_page(Preferences *p)
{
    GtkWidget *page = new_page();

    GtkWidget *grid = add_section(page, "Placement");
    p->position = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->position), "bottom", "Bottom");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->position), "top", "Top");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->position), "left", "Left");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->position), "right", "Right");
    g_signal_connect(p->position, "changed", G_CALLBACK(on_position_changed), p);
    add_row(grid, "Position on screen", NULL, p->position);
    p->alignment = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->alignment), "start", "Start");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->alignment), "center", "Center");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->alignment), "end", "End");
    g_signal_connect(p->alignment, "changed", G_CALLBACK(on_alignment_changed), p);
    add_row(grid, "Icon alignment", "Start is left (or top on a vertical dock); End is right (or bottom)", p->alignment);
    p->monitor = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->monitor), "-1", "Primary monitor");
    GdkDisplay *display = gdk_display_get_default();
    for (int i = 0; i < gdk_display_get_n_monitors(display); i++) {
        GdkMonitor *monitor = gdk_display_get_monitor(display, i);
        GdkRectangle g;
        gdk_monitor_get_geometry(monitor, &g);
        const char *model = gdk_monitor_get_model(monitor);
        g_autofree char *id = g_strdup_printf("%d", i);
        g_autofree char *label = g_strdup_printf("%d: %s (%d×%d)", i + 1, model ? model : "Monitor", g.width, g.height);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->monitor), id, label);
    }
    g_signal_connect(p->monitor, "changed", G_CALLBACK(on_monitor_changed), p);
    add_row(grid, "Monitor", NULL, p->monitor);
    p->expand = make_switch(p, &p->cfg->expand);
    add_row(grid, "Extend to screen edges", "Stretch the bar along the whole screen edge, like a panel", p->expand);
    p->autohide = make_switch(p, &p->cfg->autohide);
    g_signal_connect(p->autohide, "notify::active", G_CALLBACK(on_autohide_changed), p);
    add_row(grid, "Automatically hide and show the dock", "Slide away until the pointer touches the screen edge", p->autohide);
    p->reserve_space = make_switch(p, &p->cfg->reserve_space);
    add_row(grid, "Reserve space", "Keep maximized windows from covering the dock (not while auto-hiding)", p->reserve_space);

    grid = add_section(page, "Spacing");
    p->spacing = make_spin(p, 0, 32, &p->cfg->spacing);
    add_row(grid, "Icon spacing", "Gap between icons; scales with the dock size", p->spacing);
    p->padding = make_spin(p, 0, 32, &p->cfg->padding);
    add_row(grid, "Bar padding", "Space around the icons inside the bar; scales with the dock size", p->padding);
    p->margin = make_spin(p, 0, 64, &p->cfg->margin);
    add_row(grid, "Distance from screen edge", NULL, p->margin);
    return scrolled(page);
}

/* ---- Apps page ----------------------------------------------------------- */

static GtkWidget *app_row(GAppInfo *info)
{
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    g_object_set(box, "margin", 6, NULL);
    GIcon *gicon = g_app_info_get_icon(info);
    GtkWidget *image = gicon ? gtk_image_new_from_gicon(gicon, GTK_ICON_SIZE_DND)
                             : gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(image), 32);
    GtkWidget *label = gtk_label_new(g_app_info_get_display_name(info));
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(row), box);
    g_object_set_data_full(G_OBJECT(row), "id", g_strdup(g_app_info_get_id(info)), g_free);
    gtk_widget_show_all(row);
    return row;
}

static int selected_index(Preferences *p)
{
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(p->apps_list));
    return row ? gtk_list_box_row_get_index(row) : -1;
}

static void update_app_buttons(Preferences *p)
{
    int index = selected_index(p);
    int n = p->shown_pinned ? (int)g_strv_length(p->shown_pinned) : 0;
    gtk_widget_set_sensitive(p->remove_button, index >= 0);
    gtk_widget_set_sensitive(p->up_button, index > 0);
    gtk_widget_set_sensitive(p->down_button, index >= 0 && index < n - 1);
}

static void rebuild_apps(Preferences *p, const char *select_id)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(p->apps_list));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(children);

    GPtrArray *shown = g_ptr_array_new();
    for (char **id = p->cfg->pinned; *id; id++) {
        g_autoptr(GAppInfo) info = app_info_lookup(*id);
        if (!info)
            continue;
        GtkWidget *row = app_row(info);
        gtk_container_add(GTK_CONTAINER(p->apps_list), row);
        if (g_strcmp0(*id, select_id) == 0)
            gtk_list_box_select_row(GTK_LIST_BOX(p->apps_list), GTK_LIST_BOX_ROW(row));
        g_ptr_array_add(shown, g_strdup(*id));
    }
    g_ptr_array_add(shown, NULL);
    g_strfreev(p->shown_pinned);
    p->shown_pinned = (char **)g_ptr_array_free(shown, FALSE);
    update_app_buttons(p);
}

/* Pin `ids` in the dock, then show them with `select_id` selected. */
static void set_pinned(Preferences *p, char **ids, const char *select_id)
{
    g_autofree char *select = g_strdup(select_id);
    p->editing_apps = TRUE;
    app_tracker_set_pinned_ids(p->tracker, (const char *const *)ids);
    p->editing_apps = FALSE;
    rebuild_apps(p, select);
}

static void move_selected(Preferences *p, int delta)
{
    int index = selected_index(p);
    int n = (int)g_strv_length(p->shown_pinned);
    if (index < 0 || index + delta < 0 || index + delta >= n)
        return;
    g_auto(GStrv) ids = g_strdupv(p->shown_pinned);
    char *tmp = ids[index];
    ids[index] = ids[index + delta];
    ids[index + delta] = tmp;
    set_pinned(p, ids, tmp);
}

static void on_up(GtkButton *b, Preferences *p) { move_selected(p, -1); }
static void on_down(GtkButton *b, Preferences *p) { move_selected(p, +1); }

static void on_remove(GtkButton *b, Preferences *p)
{
    int index = selected_index(p);
    if (index < 0)
        return;
    GPtrArray *ids = g_ptr_array_new();
    for (int i = 0; p->shown_pinned[i]; i++)
        if (i != index)
            g_ptr_array_add(ids, p->shown_pinned[i]);
    g_ptr_array_add(ids, NULL);
    /* select the next app, or the previous one if the last was removed */
    guint n_left = ids->len - 1;
    const char *next = n_left ? g_ptr_array_index(ids, MIN((guint)index, n_left - 1)) : NULL;
    set_pinned(p, (char **)ids->pdata, next);
    g_ptr_array_free(ids, TRUE);
}

static gboolean filter_apps(GtkListBoxRow *row, gpointer entry)
{
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
    if (!*text)
        return TRUE;
    g_autofree char *needle = g_utf8_casefold(text, -1);
    return strstr(g_object_get_data(G_OBJECT(row), "search"), needle) != NULL;
}

static int compare_app_names(gconstpointer a, gconstpointer b)
{
    return g_utf8_collate(g_app_info_get_display_name(*(GAppInfo **)a), g_app_info_get_display_name(*(GAppInfo **)b));
}

static void on_add_selection_changed(GtkListBox *list, GtkDialog *dialog)
{
    GList *rows = gtk_list_box_get_selected_rows(list);
    gtk_dialog_set_response_sensitive(dialog, GTK_RESPONSE_ACCEPT, rows != NULL);
    g_list_free(rows);
}

static void on_add_row_activated(GtkListBox *list, GtkListBoxRow *row, GtkDialog *dialog)
{
    gtk_dialog_response(dialog, GTK_RESPONSE_ACCEPT);
}

static void on_add_search_changed(GtkSearchEntry *entry, GtkListBox *list)
{
    gtk_list_box_invalidate_filter(list);
}

static void on_add_response(GtkDialog *dialog, int response, Preferences *p)
{
    if (response == GTK_RESPONSE_ACCEPT) {
        GtkListBox *list = g_object_get_data(G_OBJECT(dialog), "list");
        GPtrArray *ids = g_ptr_array_new();
        for (char **id = p->shown_pinned; *id; id++)
            g_ptr_array_add(ids, *id);
        GList *rows = gtk_list_box_get_selected_rows(list);
        char *last = NULL;
        for (GList *l = rows; l; l = l->next)
            g_ptr_array_add(ids, last = g_object_get_data(l->data, "id"));
        g_list_free(rows);
        g_ptr_array_add(ids, NULL);
        set_pinned(p, (char **)ids->pdata, last);
        g_ptr_array_free(ids, TRUE);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void on_add(GtkButton *button, Preferences *p)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Add Applications", GTK_WINDOW(p->window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Add", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 420, 520);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, FALSE);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_box_set_spacing(GTK_BOX(content), 8);
    g_object_set(content, "margin", 12, NULL);
    GtkWidget *search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Search applications");
    gtk_box_pack_start(GTK_BOX(content), search, FALSE, FALSE, 0);

    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_MULTIPLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list), FALSE);
    gtk_list_box_set_filter_func(GTK_LIST_BOX(list), filter_apps, search, NULL);

    GPtrArray *apps = g_ptr_array_new_with_free_func(g_object_unref);
    GList *all = app_info_list();
    for (GList *l = all; l; l = l->next) {
        GAppInfo *info = l->data;
        if (!g_strv_contains((const char *const *)p->shown_pinned, g_app_info_get_id(info)))
            g_ptr_array_add(apps, g_object_ref(info));
    }
    g_list_free_full(all, g_object_unref);
    g_ptr_array_sort(apps, compare_app_names);
    for (guint i = 0; i < apps->len; i++) {
        GAppInfo *info = g_ptr_array_index(apps, i);
        GtkWidget *row = app_row(info);
        g_autofree char *search_text = g_strconcat(g_app_info_get_display_name(info), " ", g_app_info_get_id(info), NULL);
        g_object_set_data_full(G_OBJECT(row), "search", g_utf8_casefold(search_text, -1), g_free);
        gtk_container_add(GTK_CONTAINER(list), row);
    }
    g_ptr_array_free(apps, TRUE);

    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), list);
    gtk_container_add(GTK_CONTAINER(frame), sw);
    gtk_box_pack_start(GTK_BOX(content), frame, TRUE, TRUE, 0);

    g_object_set_data(G_OBJECT(dialog), "list", list);
    g_signal_connect(search, "search-changed", G_CALLBACK(on_add_search_changed), list);
    g_signal_connect(list, "selected-rows-changed", G_CALLBACK(on_add_selection_changed), dialog);
    g_signal_connect(list, "row-activated", G_CALLBACK(on_add_row_activated), dialog);
    g_signal_connect(dialog, "response", G_CALLBACK(on_add_response), p);
    gtk_widget_show_all(dialog);
}

static GtkWidget *tool_button(const char *icon, const char *tooltip, GCallback callback, Preferences *p)
{
    GtkWidget *button = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(button, tooltip);
    g_signal_connect(button, "clicked", callback, p);
    return button;
}

static void on_apps_selection_changed(GtkListBox *list, GtkListBoxRow *row, Preferences *p)
{
    update_app_buttons(p);
}

static GtkWidget *build_apps_page(Preferences *p)
{
    GtkWidget *page = new_page();
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), "<b>Pinned applications</b>");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(page), label, FALSE, FALSE, 0);
    GtkWidget *hint = gtk_label_new("Pinned apps stay in the dock even when they are not running.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(hint), "dim-label");
    gtk_box_pack_start(GTK_BOX(page), hint, FALSE, FALSE, 0);

    p->apps_list = gtk_list_box_new();
    g_signal_connect(p->apps_list, "row-selected", G_CALLBACK(on_apps_selection_changed), p);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), p->apps_list);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar), "linked");
    p->up_button = tool_button("go-up-symbolic", "Move up", G_CALLBACK(on_up), p);
    p->down_button = tool_button("go-down-symbolic", "Move down", G_CALLBACK(on_down), p);
    p->remove_button = tool_button("list-remove-symbolic", "Remove from dock", G_CALLBACK(on_remove), p);
    GtkWidget *add = gtk_button_new_with_mnemonic("_Add Application…");
    g_signal_connect(add, "clicked", G_CALLBACK(on_add), p);
    gtk_box_pack_start(GTK_BOX(toolbar), add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), p->remove_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), p->up_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), p->down_button, FALSE, FALSE, 0);

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_container_add(GTK_CONTAINER(frame), sw);
    gtk_box_pack_start(GTK_BOX(page), frame, TRUE, TRUE, 4);
    gtk_box_pack_start(GTK_BOX(page), toolbar, FALSE, FALSE, 0);
    return page;
}

/* ---- window -------------------------------------------------------------- */

static void on_destroy(GtkWidget *window, Preferences *p)
{
    if (p->save_id) {
        g_source_remove(p->save_id);
        save_now(p);
    }
    g_strfreev(p->shown_pinned);
    p->cb.closed(p->cb.data);
    g_free(p);
}

static void on_close(GtkButton *button, Preferences *p)
{
    gtk_widget_destroy(p->window);
}

Preferences *preferences_new(GtkApplication *app, DockConfig *cfg, AppTracker *tracker,
                             const PreferencesCallbacks *callbacks)
{
    Preferences *p = g_new0(Preferences, 1);
    p->cfg = cfg;
    p->tracker = tracker;
    p->cb = *callbacks;
    p->zoom_value = cfg->max_scale > 1.0 ? cfg->max_scale : 1.8;

    p->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_application(GTK_WINDOW(p->window), app);
    gtk_window_set_title(GTK_WINDOW(p->window), "Dock Settings");
    gtk_window_set_icon_name(GTK_WINDOW(p->window), "preferences-desktop");
    gtk_window_set_default_size(GTK_WINDOW(p->window), 560, 620);
    gtk_window_set_position(GTK_WINDOW(p->window), GTK_WIN_POS_CENTER);

    GtkWidget *stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_add_titled(GTK_STACK(stack), build_appearance_page(p), "appearance", "Appearance");
    gtk_stack_add_titled(GTK_STACK(stack), build_position_page(p), "position", "Position");
    gtk_stack_add_titled(GTK_STACK(stack), build_apps_page(p), "apps", "Apps");
    GtkWidget *switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(stack));
    gtk_widget_set_halign(switcher, GTK_ALIGN_CENTER);
    g_object_set(switcher, "margin-top", 12, NULL);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_object_set(buttons, "margin", 12, NULL);
    GtkWidget *reset = gtk_button_new_with_mnemonic("_Reset to Defaults");
    gtk_widget_set_tooltip_text(reset, "Restore every setting except the pinned apps");
    g_signal_connect(reset, "clicked", G_CALLBACK(on_reset), p);
    GtkWidget *close = gtk_button_new_with_mnemonic("_Close");
    g_signal_connect(close, "clicked", G_CALLBACK(on_close), p);
    gtk_box_pack_start(GTK_BOX(buttons), reset, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), close, FALSE, FALSE, 0);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox), switcher, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), buttons, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(p->window), vbox);

    load_values(p);
    rebuild_apps(p, NULL);
    g_signal_connect(p->window, "destroy", G_CALLBACK(on_destroy), p);
    gtk_widget_show_all(p->window);
    return p;
}

void preferences_present(Preferences *p, guint32 timestamp)
{
    gtk_window_present_with_time(GTK_WINDOW(p->window), timestamp);
}

void preferences_sync(Preferences *p)
{
    if (p->editing_apps || g_strv_equal((const char *const *)p->cfg->pinned, (const char *const *)p->shown_pinned))
        return;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(p->apps_list));
    g_autofree char *selected = row ? g_strdup(g_object_get_data(G_OBJECT(row), "id")) : NULL;
    rebuild_apps(p, selected);
}
