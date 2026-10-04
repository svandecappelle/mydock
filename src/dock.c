#include "dock.h"

#include <X11/Xatom.h>
#include <gdk/gdkx.h>
#include <math.h>
#include <string.h>

#include "apps.h"

#define LABEL_SPACE 44      /* room above the tallest icon for the name tooltip */
#define BOUNCE_HEIGHT 0.5   /* bounce height, as a fraction of the icon size */
#define BOUNCE_PERIOD 0.6   /* seconds per bounce */
#define BOUNCE_COUNT 2
#define ZOOM_SPEED 14.0     /* higher = snappier zoom-in/out */

typedef double Rgba[4];

typedef struct {
    Rgba bar_top, bar_bottom, border, highlight, shadow, separator, indicator, label_bg, label_fg;
} Theme;

static const Theme theme_dark = {
    .bar_top = { 0.24, 0.24, 0.26, 0.55 },
    .bar_bottom = { 0.13, 0.13, 0.15, 0.65 },
    .border = { 1, 1, 1, 0.18 },
    .highlight = { 1, 1, 1, 0.12 },
    .shadow = { 0, 0, 0, 0.05 },
    .separator = { 1, 1, 1, 0.28 },
    .indicator = { 1, 1, 1, 0.85 },
    .label_bg = { 0.14, 0.14, 0.16, 0.88 },
    .label_fg = { 1, 1, 1, 0.95 },
};

static const Theme theme_light = {
    .bar_top = { 1, 1, 1, 0.55 },
    .bar_bottom = { 0.90, 0.90, 0.93, 0.62 },
    .border = { 1, 1, 1, 0.55 },
    .highlight = { 1, 1, 1, 0.45 },
    .shadow = { 0, 0, 0, 0.04 },
    .separator = { 0, 0, 0, 0.22 },
    .indicator = { 0.1, 0.1, 0.1, 0.8 },
    .label_bg = { 0.96, 0.96, 0.97, 0.92 },
    .label_fg = { 0.08, 0.08, 0.08, 0.95 },
};

/* Where one dock entry is drawn this frame. */
typedef struct {
    DockItem *item; /* NULL for the separator */
    double x, width;
    double scale;
    double lift;    /* bounce offset in px */
} Slot;

struct Dock {
    DockConfig *cfg;
    const Theme *colors;
    AppTracker *tracker;
    GtkWidget *window, *area;
    GdkRectangle monitor_geo;

    /* Interaction / animation state. */
    double mouse_x;       /* last pointer x, kept after leaving so the dock shrinks in place */
    gboolean has_mouse_x;
    double zoom;          /* 0 = at rest, 1 = fully magnified */
    double zoom_target;
    DockItem *hovered, *pressed;
    GArray *slots;        /* Slot */
    double bar_x, bar_y, bar_w;
    guint tick_id;
    gint64 last_frame;
    cairo_rectangle_int_t input_rect;
    GtkWidget *menu;
    DockItem *menu_item;
};

static inline void set_rgba(cairo_t *cr, const Rgba c)
{
    cairo_set_source_rgba(cr, c[0], c[1], c[2], c[3]);
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    r = MIN(r, MIN(w / 2, h / 2));
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

/* ---- geometry ------------------------------------------------------------ */

static int bar_height(Dock *d)
{
    return d->cfg->icon_size + 2 * d->cfg->padding;
}

static int window_height(Dock *d)
{
    double s = d->cfg->icon_size;
    return (int)(d->cfg->margin + bar_height(d) + s * (d->cfg->max_scale - 1) + s * BOUNCE_HEIGHT + LABEL_SPACE);
}

static double bar_bottom(Dock *d)
{
    return window_height(d) - d->cfg->margin;
}

static GdkMonitor *dock_monitor(void)
{
    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    return monitor ? monitor : gdk_display_get_monitor(display, 0);
}

/* Ask the window manager to keep the bar's strip at the bottom of the screen
 * free (_NET_WM_STRUT_PARTIAL); GTK3 has no API for it. */
static void update_strut(Dock *d)
{
    GdkWindow *gdk_window = gtk_widget_get_window(d->window);
    if (!gdk_window || !d->cfg->reserve_space)
        return;
    GdkDisplay *display = gdk_display_get_default();
    int screen_bottom = 0;
    for (int i = 0; i < gdk_display_get_n_monitors(display); i++) {
        GdkRectangle g;
        gdk_monitor_get_geometry(gdk_display_get_monitor(display, i), &g);
        screen_bottom = MAX(screen_bottom, g.y + g.height);
    }
    const GdkRectangle *g = &d->monitor_geo;
    long sf = gtk_widget_get_scale_factor(d->window);
    long height = (screen_bottom - (g->y + g->height) + bar_height(d) + d->cfg->margin) * sf;
    long partial[12] = { 0, 0, 0, height, 0, 0, 0, 0, 0, 0, g->x * sf, (g->x + g->width) * sf - 1 };
    long simple[4] = { 0, 0, 0, height };

    Display *dpy = GDK_WINDOW_XDISPLAY(gdk_window);
    Window xid = GDK_WINDOW_XID(gdk_window);
    XChangeProperty(dpy, xid, XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False), XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)partial, 12);
    XChangeProperty(dpy, xid, XInternAtom(dpy, "_NET_WM_STRUT", False), XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)simple, 4);
}

static void place(Dock *d)
{
    GdkMonitor *monitor = NULL;
    if (d->cfg->monitor >= 0)
        monitor = gdk_display_get_monitor(gdk_display_get_default(), d->cfg->monitor);
    if (!monitor)
        monitor = dock_monitor();
    gdk_monitor_get_geometry(monitor, &d->monitor_geo);

    const GdkRectangle *g = &d->monitor_geo;
    int height = window_height(d);
    gtk_widget_set_size_request(d->window, g->width, height);
    gtk_window_resize(GTK_WINDOW(d->window), g->width, height);
    gtk_window_move(GTK_WINDOW(d->window), g->x, g->y + g->height - height);
    update_strut(d);
}

static double bounce_offset(Dock *d, DockItem *item, gint64 now)
{
    if (!item->bounce_start)
        return 0;
    double t = (now - item->bounce_start) / 1e6;
    if (t >= BOUNCE_PERIOD * BOUNCE_COUNT) {
        item->bounce_start = 0;
        return 0;
    }
    double phase = fmod(t, BOUNCE_PERIOD) / BOUNCE_PERIOD;
    return sin(G_PI * phase) * d->cfg->icon_size * BOUNCE_HEIGHT;
}

/* Compute this frame's slots. Each icon's scale depends on its distance to
 * the pointer, measured on the *unmagnified* layout so it is stable; then the
 * magnified row is re-centred, so the bar grows to both sides. */
static void layout(Dock *d)
{
    const DockConfig *cfg = d->cfg;
    double s = cfg->icon_size;
    GPtrArray *items = app_tracker_items(d->tracker);

    g_array_set_size(d->slots, 0);
    for (guint i = 0; i < items->len; i++) {
        DockItem *item = g_ptr_array_index(items, i);
        if (i > 0 && ((DockItem *)g_ptr_array_index(items, i - 1))->pinned && !item->pinned) {
            Slot sep = { .item = NULL, .width = MAX(2, cfg->icon_size / 4), .scale = 1 };
            g_array_append_val(d->slots, sep);
        }
        Slot slot = { .item = item, .width = s, .scale = 1 };
        g_array_append_val(d->slots, slot);
    }
    guint n = d->slots->len;
    double gaps = cfg->spacing * (n > 0 ? n - 1 : 0);

    double center = d->monitor_geo.width / 2.0;
    double base_total = gaps;
    for (guint i = 0; i < n; i++)
        base_total += g_array_index(d->slots, Slot, i).width;

    double x = center - base_total / 2;
    gint64 now = g_get_monotonic_time();
    for (guint i = 0; i < n; i++) {
        Slot *sl = &g_array_index(d->slots, Slot, i);
        double base_center = x + sl->width / 2;
        x += sl->width + cfg->spacing;
        if (!sl->item)
            continue;
        if (d->has_mouse_x && d->zoom > 0) {
            double dist = fabs(d->mouse_x - base_center) / (s + cfg->spacing);
            double falloff = dist < cfg->magnify_range ? (cos(G_PI * dist / cfg->magnify_range) + 1) / 2 : 0;
            sl->scale = 1 + (cfg->max_scale - 1) * falloff * d->zoom;
            sl->width = s * sl->scale;
        }
        sl->lift = bounce_offset(d, sl->item, now);
    }

    double total = gaps;
    for (guint i = 0; i < n; i++)
        total += g_array_index(d->slots, Slot, i).width;
    x = center - total / 2;
    for (guint i = 0; i < n; i++) {
        Slot *sl = &g_array_index(d->slots, Slot, i);
        sl->x = x;
        x += sl->width + cfg->spacing;
    }

    d->bar_w = MAX(total, s) + 2 * cfg->padding;
    d->bar_x = center - d->bar_w / 2;
    d->bar_y = bar_bottom(d) - bar_height(d);
}

static void icon_rect(Dock *d, const Slot *sl, double *x, double *y, double *size)
{
    *size = d->cfg->icon_size * sl->scale;
    *x = sl->x;
    *y = bar_bottom(d) - d->cfg->padding - *size - sl->lift;
}

static Slot *slot_at(Dock *d, double x)
{
    double half_gap = d->cfg->spacing / 2.0;
    for (guint i = 0; i < d->slots->len; i++) {
        Slot *sl = &g_array_index(d->slots, Slot, i);
        if (sl->item && sl->x - half_gap <= x && x < sl->x + sl->width + half_gap)
            return sl;
    }
    return NULL;
}

static Slot *slot_for_item(Dock *d, DockItem *item)
{
    for (guint i = 0; i < d->slots->len; i++) {
        Slot *sl = &g_array_index(d->slots, Slot, i);
        if (sl->item == item)
            return sl;
    }
    return NULL;
}

/* Only the visible dock takes clicks; the rest of the window lets the
 * pointer through to what is underneath. */
static void update_input_shape(Dock *d)
{
    if (!gtk_widget_get_realized(d->window))
        return;
    double top = d->bar_y;
    if (d->zoom > 0.01) {
        for (guint i = 0; i < d->slots->len; i++) {
            Slot *sl = &g_array_index(d->slots, Slot, i);
            double x, y, size;
            if (!sl->item)
                continue;
            icon_rect(d, sl, &x, &y, &size);
            top = MIN(top, y);
        }
    }
    cairo_rectangle_int_t rect = {
        (int)d->bar_x, (int)top, (int)ceil(d->bar_w) + 1, window_height(d) - (int)top,
    };
    if (memcmp(&rect, &d->input_rect, sizeof rect) == 0)
        return;
    d->input_rect = rect;
    cairo_region_t *region = cairo_region_create_rectangle(&rect);
    gtk_widget_input_shape_combine_region(d->window, region);
    cairo_region_destroy(region);
}

/* ---- animation ----------------------------------------------------------- */

static void refresh(Dock *d)
{
    layout(d);
    update_input_shape(d);
    gtk_widget_queue_draw(d->area);
}

static gboolean any_bouncing(Dock *d)
{
    GPtrArray *items = app_tracker_items(d->tracker);
    for (guint i = 0; i < items->len; i++)
        if (((DockItem *)g_ptr_array_index(items, i))->bounce_start)
            return TRUE;
    return FALSE;
}

static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    Dock *d = data;
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    double dt = d->last_frame ? (now - d->last_frame) / 1e6 : 1 / 60.0;
    d->last_frame = now;
    d->zoom += (d->zoom_target - d->zoom) * (1 - exp(-dt * ZOOM_SPEED));
    if (fabs(d->zoom - d->zoom_target) < 0.002)
        d->zoom = d->zoom_target;
    refresh(d);
    if (d->zoom == d->zoom_target && !any_bouncing(d)) {
        d->tick_id = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void animate(Dock *d)
{
    if (!d->tick_id) {
        d->last_frame = 0;
        d->tick_id = gtk_widget_add_tick_callback(d->area, on_tick, d, NULL);
    }
}

/* ---- drawing ------------------------------------------------------------- */

static gboolean menu_open(Dock *d)
{
    return d->menu && gtk_widget_get_visible(d->menu);
}

static void draw_bar(Dock *d, cairo_t *cr)
{
    const Theme *c = d->colors;
    double r = d->cfg->corner_radius, x = d->bar_x, y = d->bar_y, w = d->bar_w, h = bar_height(d);

    /* Soft drop shadow: a few stacked, growing translucent shapes. */
    for (int i = 1; i <= 6; i++) {
        rounded_rect(cr, x - i, y - i + 3, w + 2 * i, h + 2 * i, r + i);
        set_rgba(cr, c->shadow);
        cairo_fill(cr);
    }

    rounded_rect(cr, x, y, w, h, r);
    cairo_pattern_t *gradient = cairo_pattern_create_linear(0, y, 0, y + h);
    cairo_pattern_add_color_stop_rgba(gradient, 0, c->bar_top[0], c->bar_top[1], c->bar_top[2], c->bar_top[3]);
    cairo_pattern_add_color_stop_rgba(gradient, 1, c->bar_bottom[0], c->bar_bottom[1], c->bar_bottom[2], c->bar_bottom[3]);
    cairo_set_source(cr, gradient);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(gradient);
    cairo_set_line_width(cr, 1);
    set_rgba(cr, c->border);
    cairo_stroke(cr);

    /* Glassy highlight along the top edge. */
    rounded_rect(cr, x + 1.5, y + 1.5, w - 3, h - 3, r - 1.5);
    cairo_pattern_t *shine = cairo_pattern_create_linear(0, y, 0, y + h * 0.5);
    cairo_pattern_add_color_stop_rgba(shine, 0, c->highlight[0], c->highlight[1], c->highlight[2], c->highlight[3]);
    cairo_pattern_add_color_stop_rgba(shine, 1, c->highlight[0], c->highlight[1], c->highlight[2], 0);
    cairo_set_source(cr, shine);
    cairo_stroke(cr);
    cairo_pattern_destroy(shine);
}

static void draw_separator(Dock *d, cairo_t *cr, const Slot *sl)
{
    double h = bar_height(d);
    double x = round(sl->x + sl->width / 2) + 0.5;
    set_rgba(cr, d->colors->separator);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, x, d->bar_y + h * 0.18);
    cairo_line_to(cr, x, d->bar_y + h * 0.82);
    cairo_stroke(cr);
}

static void draw_icon(Dock *d, cairo_t *cr, const Slot *sl)
{
    DockItem *item = sl->item;
    int sf = gtk_widget_get_scale_factor(d->area);
    double x, y, size;
    icon_rect(d, sl, &x, &y, &size);

    gboolean at_rest = sl->scale < 1.02;
    GdkPixbuf *pixbuf;
    if (at_rest) { /* pixel-exact icon when not zoomed, so it stays crisp */
        x = round(x);
        y = round(y);
        size = d->cfg->icon_size;
        pixbuf = dock_item_icon(item, d->cfg->icon_size * sf);
    } else {
        pixbuf = dock_item_icon(item, (int)ceil(d->cfg->icon_size * d->cfg->max_scale * sf));
    }

    cairo_save(cr);
    cairo_translate(cr, x, y);
    double k = size / MAX(gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf));
    cairo_scale(cr, k, k);
    gdk_cairo_set_source_pixbuf(cr, pixbuf, 0, 0);
    if (!at_rest)
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
    cairo_paint(cr);
    if (item == d->pressed) { /* darken the icon while the button is held */
        cairo_pattern_t *mask = cairo_pattern_reference(cairo_get_source(cr));
        cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
        cairo_mask(cr, mask);
        cairo_pattern_destroy(mask);
    }
    cairo_restore(cr);

    if (item->windows) {
        double cx = sl->x + sl->width / 2;
        double cy = bar_bottom(d) - d->cfg->padding / 2.0;
        double radius = app_tracker_is_active(d->tracker, item) ? 2.5 : 2.0;
        cairo_arc(cr, cx, cy, radius, 0, 2 * G_PI);
        set_rgba(cr, d->colors->indicator);
        cairo_fill(cr);
    }
}

static void draw_label(Dock *d, cairo_t *cr, const Slot *sl)
{
    PangoLayout *pl = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_from_string("Sans 10");
    pango_layout_set_font_description(pl, font);
    pango_font_description_free(font);
    pango_layout_set_text(pl, dock_item_name(sl->item), -1);
    PangoRectangle logical;
    pango_layout_get_pixel_extents(pl, NULL, &logical);

    const double pad_x = 10, pad_y = 5, arrow = 6;
    double w = logical.width + 2 * pad_x, h = logical.height + 2 * pad_y;
    double ix, icon_top, size;
    icon_rect(d, sl, &ix, &icon_top, &size);
    double cx = sl->x + sl->width / 2;
    double x = CLAMP(cx - w / 2, 4, d->monitor_geo.width - w - 4);
    double y = icon_top - h - arrow - 4;

    rounded_rect(cr, x, y, w, h, 6);
    cairo_move_to(cr, cx - arrow, y + h);
    cairo_line_to(cr, cx, y + h + arrow);
    cairo_line_to(cr, cx + arrow, y + h);
    cairo_close_path(cr);
    set_rgba(cr, d->colors->label_bg);
    cairo_fill(cr);

    cairo_move_to(cr, x + pad_x - logical.x, y + pad_y - logical.y);
    set_rgba(cr, d->colors->label_fg);
    pango_cairo_show_layout(cr, pl);
    g_object_unref(pl);
}

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, Dock *d)
{
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    layout(d);
    draw_bar(d, cr);
    for (guint i = 0; i < d->slots->len; i++) {
        const Slot *sl = &g_array_index(d->slots, Slot, i);
        if (sl->item)
            draw_icon(d, cr, sl);
        else
            draw_separator(d, cr, sl);
    }
    if (d->cfg->show_labels && d->hovered && d->zoom > 0.5 && !menu_open(d)) {
        const Slot *sl = slot_for_item(d, d->hovered);
        if (sl)
            draw_label(d, cr, sl);
    }
    return TRUE;
}

/* ---- context menu -------------------------------------------------------- */

static void on_menu_window(GtkMenuItem *mi, Dock *d)
{
    WnckWindow *w = g_object_get_data(G_OBJECT(mi), "window");
    if (app_tracker_has_window(d->tracker, w)) /* it may have closed meanwhile */
        app_tracker_activate_window(d->tracker, w, gtk_get_current_event_time());
}

static void on_menu_new_window(GtkMenuItem *mi, Dock *d)
{
    if (d->menu_item)
        app_tracker_launch(d->tracker, d->menu_item, gtk_get_current_event_time());
}

static void on_menu_keep(GtkCheckMenuItem *mi, Dock *d)
{
    if (d->menu_item)
        app_tracker_set_pinned(d->tracker, d->menu_item, gtk_check_menu_item_get_active(mi));
}

static void on_menu_quit(GtkMenuItem *mi, Dock *d)
{
    if (d->menu_item)
        app_tracker_close_all(d->tracker, d->menu_item, gtk_get_current_event_time());
}

static void on_menu_closed(GtkMenuShell *menu, Dock *d)
{
    /* Shrink back unless the pointer is still over the dock. */
    GdkDevice *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
    int px, py;
    gdk_window_get_device_position(gtk_widget_get_window(d->area), pointer, &px, &py, NULL);
    cairo_rectangle_int_t *r = &d->input_rect;
    if (!(px >= r->x && px < r->x + r->width && py >= r->y && py < r->y + r->height)) {
        d->zoom_target = 0;
        d->hovered = NULL;
    }
    animate(d);
}

static void menu_append(GtkWidget *menu, const char *label, GCallback callback, Dock *d)
{
    GtkWidget *mi = gtk_menu_item_new_with_label(label);
    g_signal_connect(mi, "activate", callback, d);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

static void show_menu(Dock *d, const Slot *sl, GdkEvent *event)
{
    DockItem *item = sl->item;
    if (d->menu)
        gtk_widget_destroy(d->menu);
    d->menu = gtk_menu_new();
    d->menu_item = item;

    GList *windows = app_tracker_stacked_windows(d->tracker, item);
    for (GList *l = g_list_last(windows); l; l = l->prev) {
        const char *title = wnck_window_get_name(l->data);
        g_autofree char *head = g_utf8_substring(title, 0, 49);
        g_autofree char *label = g_utf8_strlen(title, -1) > 50 ? g_strconcat(head, "…", NULL) : g_strdup(title);
        GtkWidget *mi = gtk_menu_item_new_with_label(label);
        g_object_set_data(G_OBJECT(mi), "window", l->data);
        g_signal_connect(mi, "activate", G_CALLBACK(on_menu_window), d);
        gtk_menu_shell_append(GTK_MENU_SHELL(d->menu), mi);
    }
    if (windows)
        gtk_menu_shell_append(GTK_MENU_SHELL(d->menu), gtk_separator_menu_item_new());
    if (item->app_info) {
        menu_append(d->menu, "New Window", G_CALLBACK(on_menu_new_window), d);
        GtkWidget *keep = gtk_check_menu_item_new_with_label("Keep in Dock");
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(keep), item->pinned);
        g_signal_connect(keep, "toggled", G_CALLBACK(on_menu_keep), d);
        gtk_menu_shell_append(GTK_MENU_SHELL(d->menu), keep);
    }
    if (windows) {
        gtk_menu_shell_append(GTK_MENU_SHELL(d->menu), gtk_separator_menu_item_new());
        menu_append(d->menu, "Quit", G_CALLBACK(on_menu_quit), d);
    }
    g_list_free(windows);

    g_signal_connect(d->menu, "deactivate", G_CALLBACK(on_menu_closed), d);
    gtk_menu_attach_to_widget(GTK_MENU(d->menu), d->area, NULL);
    gtk_widget_show_all(d->menu);
    double x, y, size;
    icon_rect(d, sl, &x, &y, &size);
    GdkRectangle rect = { (int)x, (int)y - 4, (int)size, 1 };
    gtk_menu_popup_at_rect(GTK_MENU(d->menu), gtk_widget_get_window(d->window), &rect,
                           GDK_GRAVITY_NORTH, GDK_GRAVITY_SOUTH, event);
    gtk_widget_queue_draw(d->area);
}

/* ---- input --------------------------------------------------------------- */

static gboolean on_motion(GtkWidget *widget, GdkEvent *event, Dock *d)
{
    double x;
    gdk_event_get_coords(event, &x, NULL);
    d->mouse_x = x;
    d->has_mouse_x = TRUE;
    d->zoom_target = 1;
    Slot *sl = slot_at(d, x);
    d->hovered = sl ? sl->item : NULL;
    refresh(d);
    animate(d);
    return FALSE;
}

static gboolean on_leave(GtkWidget *widget, GdkEventCrossing *event, Dock *d)
{
    if (menu_open(d))
        return FALSE;
    d->zoom_target = 0;
    d->hovered = NULL;
    d->pressed = NULL;
    animate(d);
    return FALSE;
}

static gboolean on_press(GtkWidget *widget, GdkEventButton *event, Dock *d)
{
    if (event->type != GDK_BUTTON_PRESS) /* ignore double/triple-click events */
        return FALSE;
    Slot *sl = slot_at(d, event->x);
    if (!sl)
        return FALSE;
    if (event->button == GDK_BUTTON_SECONDARY) {
        show_menu(d, sl, (GdkEvent *)event);
    } else {
        d->pressed = sl->item;
        gtk_widget_queue_draw(d->area);
    }
    return TRUE;
}

static gboolean on_release(GtkWidget *widget, GdkEventButton *event, Dock *d)
{
    DockItem *pressed = d->pressed;
    d->pressed = NULL;
    Slot *sl = slot_at(d, event->x);
    if (pressed && sl && sl->item == pressed) {
        if (event->button == GDK_BUTTON_PRIMARY)
            app_tracker_activate(d->tracker, pressed, event->time);
        else if (event->button == GDK_BUTTON_MIDDLE)
            app_tracker_launch(d->tracker, pressed, event->time);
    }
    gtk_widget_queue_draw(d->area);
    return TRUE;
}

/* ---- model updates ------------------------------------------------------- */

static void on_items_changed(gpointer data)
{
    Dock *d = data;
    refresh(d);
    animate(d); /* runs launch bounces; stops by itself when idle */
}

static void on_item_removed(DockItem *item, gpointer data)
{
    Dock *d = data;
    if (d->hovered == item)
        d->hovered = NULL;
    if (d->pressed == item)
        d->pressed = NULL;
    if (d->menu_item == item) {
        d->menu_item = NULL;
        if (d->menu)
            gtk_menu_popdown(GTK_MENU(d->menu));
    }
    /* Slots may still point at the item until the next layout. */
    g_array_set_size(d->slots, 0);
}

static void on_icon_theme_changed(GtkIconTheme *theme, Dock *d)
{
    GPtrArray *items = app_tracker_items(d->tracker);
    for (guint i = 0; i < items->len; i++)
        dock_item_clear_icons(g_ptr_array_index(items, i));
    gtk_widget_queue_draw(d->area);
}

static void on_realize(GtkWidget *widget, Dock *d)
{
    update_strut(d);
    refresh(d);
}

static void on_monitors_changed(GdkScreen *screen, Dock *d)
{
    place(d);
    refresh(d);
}

/* ---- construction -------------------------------------------------------- */

Dock *dock_new(GtkApplication *app, DockConfig *cfg)
{
    Dock *d = g_new0(Dock, 1);
    d->cfg = cfg;
    d->colors = g_strcmp0(cfg->theme, "light") == 0 ? &theme_light : &theme_dark;
    d->slots = g_array_new(FALSE, TRUE, sizeof(Slot));

    GtkWindow *win = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    d->window = GTK_WIDGET(win);
    gtk_window_set_application(win, app);
    gtk_window_set_title(win, "macdock");
    gtk_window_set_type_hint(win, GDK_WINDOW_TYPE_HINT_DOCK);
    gtk_window_set_decorated(win, FALSE);
    gtk_window_set_skip_taskbar_hint(win, TRUE);
    gtk_window_set_skip_pager_hint(win, TRUE);
    gtk_window_set_keep_above(win, TRUE);
    gtk_window_set_accept_focus(win, FALSE);
    gtk_window_stick(win);
    gtk_widget_set_app_paintable(d->window, TRUE);
    GdkScreen *screen = gtk_widget_get_screen(d->window);
    GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
    if (visual && gdk_screen_is_composited(screen))
        gtk_widget_set_visual(d->window, visual);
    else
        g_warning("no compositor running, transparency disabled");

    AppTrackerCallbacks callbacks = { on_items_changed, on_item_removed, d };
    d->tracker = app_tracker_new(cfg, &callbacks);

    d->area = gtk_drawing_area_new();
    gtk_widget_add_events(d->area, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                       | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(d->area, "draw", G_CALLBACK(on_draw), d);
    g_signal_connect(d->area, "motion-notify-event", G_CALLBACK(on_motion), d);
    g_signal_connect(d->area, "enter-notify-event", G_CALLBACK(on_motion), d);
    g_signal_connect(d->area, "leave-notify-event", G_CALLBACK(on_leave), d);
    g_signal_connect(d->area, "button-press-event", G_CALLBACK(on_press), d);
    g_signal_connect(d->area, "button-release-event", G_CALLBACK(on_release), d);
    gtk_container_add(GTK_CONTAINER(win), d->area);

    g_signal_connect(d->window, "realize", G_CALLBACK(on_realize), d);
    g_signal_connect(screen, "monitors-changed", G_CALLBACK(on_monitors_changed), d);
    g_signal_connect(gtk_icon_theme_get_default(), "changed", G_CALLBACK(on_icon_theme_changed), d);
    place(d);
    return d;
}

GtkWidget *dock_get_window(Dock *d)
{
    return d->window;
}
