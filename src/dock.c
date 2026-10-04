#include "dock.h"

#include <X11/Xatom.h>
#include <gdk/gdkx.h>
#include <math.h>
#include <string.h>

#include "apps.h"
#include "preferences.h"

#define LABEL_SPACE 44      /* room beyond the tallest icon for the name label */
#define LABEL_SPACE_SIDE 240 /* same, beside the icons of a vertical dock */
#define BOUNCE_HEIGHT 0.5   /* bounce height, as a fraction of the icon size */
#define BOUNCE_PERIOD 0.6   /* seconds per bounce */
#define BOUNCE_COUNT 2
#define ZOOM_SPEED 14.0     /* higher = snappier zoom-in/out */
#define SCREEN_END_GAP 8    /* minimum room left at both ends of a full dock */
#define MIN_ICON_SIZE 16
#define HIDE_DELAY_MS 500   /* auto-hide: wait this long after the pointer leaves */
#define HIDE_SPEED 10.0     /* auto-hide slide speed, like ZOOM_SPEED */
#define TRIGGER_SIZE 2      /* auto-hide: depth of the strip at the screen edge that reveals the dock */
#define DRAG_THRESHOLD 8    /* pointer travel (px) that turns a press on an icon into a drag */

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

/* Where one dock entry is drawn this frame, in dock coordinates (see
 * to_window): `pos` and `len` run along the dock. */
typedef enum {
    SLOT_ICON,
    SLOT_SEPARATOR, /* between pinned and other running apps */
    SLOT_GAP,       /* where the icon being dragged will land */
} SlotKind;

typedef struct {
    SlotKind kind;
    DockItem *item; /* SLOT_ICON only */
    double pos, len;
    double scale;
    double lift;    /* bounce offset in px, away from the screen edge */
} Slot;

typedef struct {
    double x, y, w, h;
} Rect;

struct Dock {
    DockConfig *cfg;
    const Theme *colors;
    AppTracker *tracker;
    GtkWidget *window, *area;
    GdkRectangle monitor_geo;

    /* Interaction / animation state. */
    double mouse_pos;     /* pointer position along the dock, kept after leaving so the dock shrinks in place */
    gboolean has_mouse_pos;
    double zoom;          /* 0 = at rest, 1 = fully magnified */
    double zoom_target;
    double hide;          /* auto-hide: 0 = shown, 1 = slid out of view */
    double hide_target;
    guint hide_timeout_id;
    gboolean pointer_inside;
    DockItem *hovered, *pressed;
    double press_x, press_y; /* where the button went down, window coordinates */
    double grab_fx, grab_fy; /* where the pressed icon was grabbed, as a fraction of its size */
    DockItem *drag_item;  /* icon being dragged to a new place, or NULL */
    double drag_x, drag_y; /* pointer during the drag, window coordinates */
    guint drop_index;     /* where it lands: index in the pinned group or the running group */
    gboolean drop_pinned;
    GArray *slots;        /* Slot */
    double bar_pos, bar_len;
    double size;          /* icon size in use: the configured size, shrunk if the dock would not fit */
    double strut_size;    /* the size the reserved screen space was computed for */
    guint tick_id;
    gint64 last_frame;
    cairo_rectangle_int_t input_rect;
    GtkWidget *menu;
    DockItem *menu_item;
    GtkApplication *app;
    Preferences *prefs;
};

static inline void set_rgba(cairo_t *cr, const Rgba c)
{
    cairo_set_source_rgba(cr, c[0], c[1], c[2], c[3]);
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    r = MAX(0, MIN(r, MIN(w / 2, h / 2)));
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

/* ---- geometry ------------------------------------------------------------ *
 *
 * Layout is computed in "dock coordinates": `a` runs along the dock (left to
 * right, or top to bottom) and `e` is the distance from the screen edge the
 * dock is attached to. to_window() maps them to window pixels for the
 * configured edge; icons and text are never rotated, only placed. */

static gboolean is_vertical(Dock *d)
{
    return d->cfg->position == DOCK_LEFT || d->cfg->position == DOCK_RIGHT;
}

/* Padding, spacing and corner radius scale with the dock size. */
static double scaled(Dock *d, int value)
{
    return round(value * d->size / DOCK_BASE_SIZE);
}

static double padding(Dock *d) { return scaled(d, d->cfg->padding); }
static double spacing(Dock *d) { return scaled(d, d->cfg->spacing); }
static double corner_radius(Dock *d) { return scaled(d, d->cfg->corner_radius); }

static double bar_thickness(Dock *d)
{
    return d->size + 2 * padding(d);
}

/* Window size across the dock: room for the bar, a magnified bouncing icon,
 * and the name label (beside the icons on a vertical dock). */
static int window_depth(Dock *d)
{
    double s = d->cfg->icon_size;
    int label = is_vertical(d) ? LABEL_SPACE_SIDE : LABEL_SPACE;
    return (int)(d->cfg->margin + bar_thickness(d) + s * (d->cfg->max_scale - 1) + s * BOUNCE_HEIGHT + label);
}

/* Window size along the dock: the monitor's width or height. */
static double dock_length(Dock *d)
{
    return is_vertical(d) ? d->monitor_geo.height : d->monitor_geo.width;
}

static Rect to_window_raw(Dock *d, double a, double e, double len, double depth)
{
    double t = window_depth(d);
    switch (d->cfg->position) {
    case DOCK_TOP:   return (Rect){ a, e, len, depth };
    case DOCK_LEFT:  return (Rect){ e, a, depth, len };
    case DOCK_RIGHT: return (Rect){ t - e - depth, a, depth, len };
    default:         return (Rect){ a, t - e - depth, len, depth };
    }
}

/* How far the dock slides toward the screen edge when auto-hidden: past the
 * edge by the bar, its margin, and some room for the shadow. */
static double hide_distance(Dock *d)
{
    return d->cfg->margin + bar_thickness(d) + 12;
}

/* Like to_window_raw(), with the auto-hide slide applied: everything drawn
 * moves toward the screen edge, out of the window. */
static Rect to_window(Dock *d, double a, double e, double len, double depth)
{
    return to_window_raw(d, a, e - d->hide * hide_distance(d), len, depth);
}

static void fit_size(Dock *d, GPtrArray *items);
static double bar_thickness(Dock *d);

static GdkMonitor *dock_monitor(void)
{
    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    return monitor ? monitor : gdk_display_get_monitor(display, 0);
}

/* Ask the window manager to keep the bar's strip along the screen edge free
 * (_NET_WM_STRUT_PARTIAL); GTK3 has no API for it. */
static void update_strut(Dock *d)
{
    GdkWindow *gdk_window = gtk_widget_get_window(d->window);
    if (!gdk_window)
        return;
    Display *dpy = GDK_WINDOW_XDISPLAY(gdk_window);
    Window xid = GDK_WINDOW_XID(gdk_window);
    d->strut_size = d->size;
    if (!d->cfg->reserve_space || d->cfg->autohide) {
        XDeleteProperty(dpy, xid, XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False));
        XDeleteProperty(dpy, xid, XInternAtom(dpy, "_NET_WM_STRUT", False));
        return;
    }
    /* Struts are relative to the edges of the whole screen, not the monitor. */
    GdkDisplay *display = gdk_display_get_default();
    long screen_w = 0, screen_h = 0;
    for (int i = 0; i < gdk_display_get_n_monitors(display); i++) {
        GdkRectangle m;
        gdk_monitor_get_geometry(gdk_display_get_monitor(display, i), &m);
        screen_w = MAX(screen_w, m.x + m.width);
        screen_h = MAX(screen_h, m.y + m.height);
    }
    const GdkRectangle *g = &d->monitor_geo;
    long sf = gtk_widget_get_scale_factor(d->window);
    long thick = (long)bar_thickness(d) + d->cfg->margin;
    /* left, right, top, bottom, then start/end pairs for each of them */
    long partial[12] = { 0 };
    switch (d->cfg->position) {
    case DOCK_TOP:
        partial[2] = g->y + thick;
        partial[8] = g->x, partial[9] = g->x + g->width - 1;
        break;
    case DOCK_LEFT:
        partial[0] = g->x + thick;
        partial[4] = g->y, partial[5] = g->y + g->height - 1;
        break;
    case DOCK_RIGHT:
        partial[1] = screen_w - (g->x + g->width) + thick;
        partial[6] = g->y, partial[7] = g->y + g->height - 1;
        break;
    default:
        partial[3] = screen_h - (g->y + g->height) + thick;
        partial[10] = g->x, partial[11] = g->x + g->width - 1;
        break;
    }
    for (int i = 0; i < 12; i++)
        partial[i] *= sf;
    XChangeProperty(dpy, xid, XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False), XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)partial, 12);
    XChangeProperty(dpy, xid, XInternAtom(dpy, "_NET_WM_STRUT", False), XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)partial, 4);
}

static void place(Dock *d)
{
    GdkMonitor *monitor = NULL;
    if (d->cfg->monitor >= 0)
        monitor = gdk_display_get_monitor(gdk_display_get_default(), d->cfg->monitor);
    if (!monitor)
        monitor = dock_monitor();
    gdk_monitor_get_geometry(monitor, &d->monitor_geo);
    fit_size(d, app_tracker_items(d->tracker));

    const GdkRectangle *g = &d->monitor_geo;
    int t = window_depth(d);
    GdkRectangle r;
    switch (d->cfg->position) {
    case DOCK_TOP:   r = (GdkRectangle){ g->x, g->y, g->width, t }; break;
    case DOCK_LEFT:  r = (GdkRectangle){ g->x, g->y, t, g->height }; break;
    case DOCK_RIGHT: r = (GdkRectangle){ g->x + g->width - t, g->y, t, g->height }; break;
    default:         r = (GdkRectangle){ g->x, g->y + g->height - t, g->width, t }; break;
    }
    gtk_widget_set_size_request(d->window, r.width, r.height);
    gtk_window_resize(GTK_WINDOW(d->window), r.width, r.height);
    gtk_window_move(GTK_WINDOW(d->window), r.x, r.y);
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
    return sin(G_PI * phase) * d->size * BOUNCE_HEIGHT;
}

/* Use the configured icon size, or the largest size at which all icons fit
 * along the screen edge, like the macOS dock. Everything in the bar scales
 * with the size, so its length is linear in it: size * per_px. */
static void fit_size(Dock *d, GPtrArray *items)
{
    const DockConfig *cfg = d->cfg;
    double n_icons = items->len, n_slots = items->len;
    for (guint i = 1; i < items->len; i++)
        if (((DockItem *)g_ptr_array_index(items, i - 1))->pinned && !((DockItem *)g_ptr_array_index(items, i))->pinned) {
            n_icons += 0.25; /* the separator is a quarter icon wide */
            n_slots += 1;
        }
    double per_px = n_icons + ((n_slots > 0 ? n_slots - 1 : 0) * cfg->spacing + 2.0 * cfg->padding) / DOCK_BASE_SIZE;
    double room = dock_length(d) - 2 * SCREEN_END_GAP;
    d->size = cfg->icon_size;
    if (per_px > 0 && d->size * per_px > room)
        d->size = MAX(MIN_ICON_SIZE, floor(room / per_px));
}

/* Where a row of icons `total` long starts along the dock, for the
 * configured alignment. At the start or end, the row keeps the bar padding
 * from the screen end (plus a small gap when the bar does not fill the edge). */
static double row_start(Dock *d, double total)
{
    double inset = padding(d) + (d->cfg->expand ? 0 : SCREEN_END_GAP);
    switch (d->cfg->alignment) {
    case ALIGN_START: return inset;
    case ALIGN_END:   return dock_length(d) - inset - total;
    default:          return (dock_length(d) - total) / 2;
    }
}

/* Compute this frame's slots. Each icon's scale depends on its distance to
 * the pointer, measured on the *unmagnified* layout so it is stable; then the
 * magnified row is placed again: centred (the bar grows to both sides) or
 * anchored at the start or end of the edge. */
static void layout(Dock *d)
{
    const DockConfig *cfg = d->cfg;
    GPtrArray *items = app_tracker_items(d->tracker);
    fit_size(d, items);
    double s = d->size, gap = spacing(d);

    /* The entries to show: items in order (pinned first), except the one being
     * dragged, which leaves a gap (NULL) at the place it would land. */
    GPtrArray *entries = g_ptr_array_new();
    guint n_pinned = 0;
    for (guint i = 0; i < items->len; i++) {
        DockItem *item = g_ptr_array_index(items, i);
        if (item == d->drag_item)
            continue;
        g_ptr_array_add(entries, item);
        n_pinned += item->pinned;
    }
    if (d->drag_item) {
        guint at = d->drop_pinned ? MIN(d->drop_index, n_pinned)
                                  : n_pinned + MIN(d->drop_index, entries->len - n_pinned);
        g_ptr_array_insert(entries, at, NULL);
    }

    g_array_set_size(d->slots, 0);
    gboolean prev_pinned = FALSE;
    for (guint i = 0; i < entries->len; i++) {
        DockItem *item = g_ptr_array_index(entries, i);
        gboolean pinned = item ? item->pinned : d->drop_pinned;
        if (i > 0 && prev_pinned && !pinned) {
            Slot sep = { .kind = SLOT_SEPARATOR, .len = MAX(2, round(s / 4)), .scale = 1 };
            g_array_append_val(d->slots, sep);
        }
        Slot slot = { .kind = item ? SLOT_ICON : SLOT_GAP, .item = item, .len = s, .scale = 1 };
        g_array_append_val(d->slots, slot);
        prev_pinned = pinned;
    }
    g_ptr_array_free(entries, TRUE);
    guint n = d->slots->len;
    double gaps = gap * (n > 0 ? n - 1 : 0);

    double base_total = gaps;
    for (guint i = 0; i < n; i++)
        base_total += g_array_index(d->slots, Slot, i).len;

    double a = row_start(d, base_total);
    gint64 now = g_get_monotonic_time();
    for (guint i = 0; i < n; i++) {
        Slot *sl = &g_array_index(d->slots, Slot, i);
        double base_center = a + sl->len / 2;
        a += sl->len + gap;
        if (sl->kind != SLOT_ICON)
            continue;
        if (d->has_mouse_pos && d->zoom > 0) {
            double dist = fabs(d->mouse_pos - base_center) / (s + gap);
            double falloff = dist < cfg->magnify_range ? (cos(G_PI * dist / cfg->magnify_range) + 1) / 2 : 0;
            sl->scale = 1 + (cfg->max_scale - 1) * falloff * d->zoom;
            sl->len = s * sl->scale;
        }
        sl->lift = bounce_offset(d, sl->item, now);
    }

    double total = gaps;
    for (guint i = 0; i < n; i++)
        total += g_array_index(d->slots, Slot, i).len;
    a = row_start(d, total); /* magnification grows the row away from its anchored end */
    for (guint i = 0; i < n; i++) {
        Slot *sl = &g_array_index(d->slots, Slot, i);
        sl->pos = a;
        a += sl->len + gap;
    }

    d->bar_len = MAX(total, s) + 2 * padding(d);
    d->bar_pos = row_start(d, MAX(total, s)) - padding(d);
    if (d->cfg->expand) {
        /* Span the whole edge. Overshoot both ends by the corner radius so the
         * rounded corners fall outside the window and the ends look square. */
        double r = corner_radius(d) + 1;
        d->bar_pos = -r;
        d->bar_len = dock_length(d) + 2 * r;
    }
    if (d->size != d->strut_size) /* apps opened or closed changed the fitted size */
        update_strut(d);
}

static Rect bar_rect(Dock *d)
{
    return to_window(d, d->bar_pos, d->cfg->margin, d->bar_len, bar_thickness(d));
}

/* Distance from the screen edge of the icon's far side. */
static double icon_far_edge(Dock *d, const Slot *sl)
{
    return d->cfg->margin + padding(d) + sl->lift + d->size * sl->scale;
}

static Rect icon_rect(Dock *d, const Slot *sl)
{
    double size = d->size * sl->scale;
    return to_window(d, sl->pos, d->cfg->margin + padding(d) + sl->lift, size, size);
}

/* Pointer position along the dock, from window coordinates. */
static double along(Dock *d, double x, double y)
{
    return is_vertical(d) ? y : x;
}

static Slot *slot_at(Dock *d, double a)
{
    double half_gap = spacing(d) / 2;
    for (guint i = 0; i < d->slots->len; i++) {
        Slot *sl = &g_array_index(d->slots, Slot, i);
        if (sl->item && sl->pos - half_gap <= a && a < sl->pos + sl->len + half_gap)
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
 * pointer through to what is underneath. The input area reaches the screen
 * edge, so the pointer can be thrown against it. */
static void update_input_shape(Dock *d)
{
    if (!gtk_widget_get_realized(d->window))
        return;
    double depth = d->cfg->margin + bar_thickness(d);
    if (d->zoom > 0.01) {
        for (guint i = 0; i < d->slots->len; i++) {
            Slot *sl = &g_array_index(d->slots, Slot, i);
            if (sl->item)
                depth = MAX(depth, icon_far_edge(d, sl));
        }
    }
    /* While auto-hidden, only a thin strip at the screen edge is left. */
    depth = MAX(TRIGGER_SIZE, depth - d->hide * hide_distance(d));
    Rect r = to_window_raw(d, d->bar_pos, 0, d->bar_len, depth);
    int x = (int)floor(r.x), y = (int)floor(r.y);
    cairo_rectangle_int_t rect = { x, y, (int)ceil(r.x + r.w) - x, (int)ceil(r.y + r.h) - y };
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
    d->hide += (d->hide_target - d->hide) * (1 - exp(-dt * HIDE_SPEED));
    if (fabs(d->hide - d->hide_target) < 0.002)
        d->hide = d->hide_target;
    refresh(d);
    if (d->zoom == d->zoom_target && d->hide == d->hide_target && !any_bouncing(d)) {
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

/* ---- auto-hide ----------------------------------------------------------- */

static void cancel_hide(Dock *d)
{
    if (d->hide_timeout_id) {
        g_source_remove(d->hide_timeout_id);
        d->hide_timeout_id = 0;
    }
}

static void show_dock(Dock *d)
{
    cancel_hide(d);
    d->hide_target = 0;
    animate(d);
}

static gboolean on_hide_timeout(gpointer data)
{
    Dock *d = data;
    d->hide_timeout_id = 0;
    d->hide_target = 1;
    animate(d);
    return G_SOURCE_REMOVE;
}

/* Hide after a short delay, unless the pointer comes back meanwhile. */
static void schedule_hide(Dock *d)
{
    if (!d->cfg->autohide)
        return;
    cancel_hide(d);
    d->hide_timeout_id = g_timeout_add(HIDE_DELAY_MS, on_hide_timeout, d);
}

/* The pointer left the dock: shrink back, and hide it if auto-hiding. */
static void pointer_left(Dock *d)
{
    d->zoom_target = 0;
    d->hovered = NULL;
    d->pressed = NULL;
    d->pointer_inside = FALSE;
    schedule_hide(d);
    animate(d);
}

static gboolean over_dock(Dock *d, double x, double y)
{
    const cairo_rectangle_int_t *r = &d->input_rect;
    return x >= r->x && x < r->x + r->width && y >= r->y && y < r->y + r->height;
}

/* ---- drawing ------------------------------------------------------------- */

static gboolean menu_open(Dock *d)
{
    return d->menu && gtk_widget_get_visible(d->menu);
}

static void draw_bar(Dock *d, cairo_t *cr)
{
    const Theme *c = d->colors;
    Rect b = bar_rect(d);
    double r = corner_radius(d);

    /* Soft drop shadow: a few stacked, growing translucent shapes. */
    for (int i = 1; i <= 6; i++) {
        rounded_rect(cr, b.x - i, b.y - i + 3, b.w + 2 * i, b.h + 2 * i, r + i);
        set_rgba(cr, c->shadow);
        cairo_fill(cr);
    }

    rounded_rect(cr, b.x, b.y, b.w, b.h, r);
    cairo_pattern_t *gradient = cairo_pattern_create_linear(0, b.y, 0, b.y + b.h);
    cairo_pattern_add_color_stop_rgba(gradient, 0, c->bar_top[0], c->bar_top[1], c->bar_top[2], c->bar_top[3]);
    cairo_pattern_add_color_stop_rgba(gradient, 1, c->bar_bottom[0], c->bar_bottom[1], c->bar_bottom[2], c->bar_bottom[3]);
    cairo_set_source(cr, gradient);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(gradient);
    cairo_set_line_width(cr, 1);
    set_rgba(cr, c->border);
    cairo_stroke(cr);

    /* Glassy highlight, strongest along the top edge. */
    rounded_rect(cr, b.x + 1.5, b.y + 1.5, b.w - 3, b.h - 3, r - 1.5);
    cairo_pattern_t *shine = cairo_pattern_create_linear(0, b.y, 0, b.y + MIN(b.h, b.w) * 0.5);
    cairo_pattern_add_color_stop_rgba(shine, 0, c->highlight[0], c->highlight[1], c->highlight[2], c->highlight[3]);
    cairo_pattern_add_color_stop_rgba(shine, 1, c->highlight[0], c->highlight[1], c->highlight[2], 0);
    cairo_set_source(cr, shine);
    cairo_stroke(cr);
    cairo_pattern_destroy(shine);
}

static void draw_separator(Dock *d, cairo_t *cr, const Slot *sl)
{
    double t = bar_thickness(d);
    Rect r = to_window(d, sl->pos + sl->len / 2, d->cfg->margin + t * 0.18, 0, t * 0.64);
    /* r is a zero-width (or zero-height) line; snap it to the pixel grid */
    if (r.w == 0)
        r.x = round(r.x) + 0.5;
    else
        r.y = round(r.y) + 0.5;
    set_rgba(cr, d->colors->separator);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, r.x, r.y);
    cairo_line_to(cr, r.x + r.w, r.y + r.h);
    cairo_stroke(cr);
}

/* Paint `item`'s icon into `r`; crisp when drawn at its rest size. */
static void paint_icon(Dock *d, cairo_t *cr, DockItem *item, Rect r, double scale, gboolean darken)
{
    int sf = gtk_widget_get_scale_factor(d->area);
    gboolean at_rest = scale < 1.02;
    GdkPixbuf *pixbuf;
    if (at_rest) { /* pixel-exact icon when not zoomed, so it stays crisp */
        r = (Rect){ round(r.x), round(r.y), d->size, d->size };
        pixbuf = dock_item_icon(item, (int)d->size * sf);
    } else {
        pixbuf = dock_item_icon(item, (int)ceil(d->size * d->cfg->max_scale * sf));
    }

    cairo_save(cr);
    cairo_translate(cr, r.x, r.y);
    double k = r.w / MAX(gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf));
    cairo_scale(cr, k, k);
    gdk_cairo_set_source_pixbuf(cr, pixbuf, 0, 0);
    if (!at_rest)
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
    cairo_paint(cr);
    if (darken) {
        cairo_pattern_t *mask = cairo_pattern_reference(cairo_get_source(cr));
        cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
        cairo_mask(cr, mask);
        cairo_pattern_destroy(mask);
    }
    cairo_restore(cr);
}

static void draw_icon(Dock *d, cairo_t *cr, const Slot *sl)
{
    DockItem *item = sl->item;
    paint_icon(d, cr, item, icon_rect(d, sl), sl->scale, item == d->pressed); /* darkened while held */

    if (item->windows) { /* running indicator, between the icon and the screen edge */
        Rect dot = to_window(d, sl->pos + sl->len / 2, d->cfg->margin + padding(d) / 2, 0, 0);
        double radius = app_tracker_is_active(d->tracker, item) ? 2.5 : 2.0;
        cairo_arc(cr, dot.x, dot.y, radius, 0, 2 * G_PI);
        set_rgba(cr, d->colors->indicator);
        cairo_fill(cr);
    }
}

/* The app name in a rounded box beside the hovered icon, on the side away
 * from the screen edge, with a small arrow pointing at the icon. */
/* Top-left corner of the dragged icon, in window coordinates: it follows the
 * pointer, held where it was grabbed. */
static void drag_icon_origin(Dock *d, double *x, double *y)
{
    *x = d->drag_x - d->grab_fx * d->size;
    *y = d->drag_y - d->grab_fy * d->size;
}

static void draw_dragged_icon(Dock *d, cairo_t *cr)
{
    double x, y;
    drag_icon_origin(d, &x, &y);
    paint_icon(d, cr, d->drag_item, (Rect){ x, y, d->size, d->size }, 1.0, FALSE);
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

    const double pad_x = 10, pad_y = 5, arrow = 6, gap = 4;
    double w = logical.width + 2 * pad_x, h = logical.height + 2 * pad_y;
    int win_w = gtk_widget_get_allocated_width(d->area), win_h = gtk_widget_get_allocated_height(d->area);
    Rect icon = icon_rect(d, sl);
    double cx = icon.x + icon.w / 2, cy = icon.y + icon.h / 2;
    double x, y, tip_x, tip_y, base_dx, base_dy; /* arrow tip, and half-base direction */

    switch (d->cfg->position) {
    case DOCK_TOP:
        x = CLAMP(cx - w / 2, 4, win_w - w - 4), y = icon.y + icon.h + arrow + gap;
        tip_x = cx, tip_y = y - arrow, base_dx = arrow, base_dy = 0;
        break;
    case DOCK_LEFT:
        x = icon.x + icon.w + arrow + gap, y = CLAMP(cy - h / 2, 4, win_h - h - 4);
        tip_x = x - arrow, tip_y = cy, base_dx = 0, base_dy = arrow;
        break;
    case DOCK_RIGHT:
        x = icon.x - w - arrow - gap, y = CLAMP(cy - h / 2, 4, win_h - h - 4);
        tip_x = x + w + arrow, tip_y = cy, base_dx = 0, base_dy = arrow;
        break;
    default:
        x = CLAMP(cx - w / 2, 4, win_w - w - 4), y = icon.y - h - arrow - gap;
        tip_x = cx, tip_y = y + h + arrow, base_dx = arrow, base_dy = 0;
        break;
    }
    /* the arrow's base sits on the box edge facing the icon */
    double base_x = tip_x + (base_dy ? (tip_x < x ? arrow : -arrow) : 0);
    double base_y = tip_y + (base_dx ? (tip_y < y ? arrow : -arrow) : 0);

    rounded_rect(cr, x, y, w, h, 6);
    cairo_move_to(cr, base_x - base_dx, base_y - base_dy);
    cairo_line_to(cr, tip_x, tip_y);
    cairo_line_to(cr, base_x + base_dx, base_y + base_dy);
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
        if (sl->kind == SLOT_ICON)
            draw_icon(d, cr, sl);
        else if (sl->kind == SLOT_SEPARATOR)
            draw_separator(d, cr, sl);
    }
    if (d->drag_item)
        draw_dragged_icon(d, cr);
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

static void on_menu_settings(GtkMenuItem *mi, Dock *d)
{
    dock_show_preferences(d, gtk_get_current_event_time());
}

static void on_menu_quit_dock(GtkMenuItem *mi, Dock *d)
{
    g_application_quit(G_APPLICATION(d->app));
}

static void on_menu_closed(GtkMenuShell *menu, Dock *d)
{
    /* Shrink back unless the pointer is still over the dock. */
    GdkDevice *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
    int px, py;
    gdk_window_get_device_position(gtk_widget_get_window(d->area), pointer, &px, &py, NULL);
    if (!over_dock(d, px, py))
        pointer_left(d);
    else
        animate(d);
}

static void menu_append(GtkWidget *menu, const char *label, GCallback callback, Dock *d)
{
    GtkWidget *mi = gtk_menu_item_new_with_label(label);
    g_signal_connect(mi, "activate", callback, d);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

/* Context menu of an icon, or of the bar itself when `sl` is NULL. */
static void show_menu(Dock *d, const Slot *sl, GdkEvent *event)
{
    if (d->menu)
        gtk_widget_destroy(d->menu);
    d->menu = gtk_menu_new();
    d->menu_item = sl ? sl->item : NULL;
    g_signal_connect(d->menu, "deactivate", G_CALLBACK(on_menu_closed), d);
    gtk_menu_attach_to_widget(GTK_MENU(d->menu), d->area, NULL);

    if (!sl) {
        menu_append(d->menu, "Dock Settings…", G_CALLBACK(on_menu_settings), d);
        gtk_menu_shell_append(GTK_MENU_SHELL(d->menu), gtk_separator_menu_item_new());
        menu_append(d->menu, "Quit Dock", G_CALLBACK(on_menu_quit_dock), d);
        gtk_widget_show_all(d->menu);
        gtk_menu_popup_at_pointer(GTK_MENU(d->menu), event);
        return;
    }

    DockItem *item = sl->item;
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
    gtk_menu_shell_append(GTK_MENU_SHELL(d->menu), gtk_separator_menu_item_new());
    menu_append(d->menu, "Dock Settings…", G_CALLBACK(on_menu_settings), d);

    gtk_widget_show_all(d->menu);
    /* Open the menu beside the icon, on the side away from the screen edge. */
    Rect r = icon_rect(d, sl);
    GdkRectangle rect = { (int)r.x, (int)r.y, (int)r.w, (int)r.h };
    GdkGravity anchor, menu_anchor;
    switch (d->cfg->position) {
    case DOCK_TOP:   anchor = GDK_GRAVITY_SOUTH, menu_anchor = GDK_GRAVITY_NORTH; break;
    case DOCK_LEFT:  anchor = GDK_GRAVITY_EAST, menu_anchor = GDK_GRAVITY_WEST; break;
    case DOCK_RIGHT: anchor = GDK_GRAVITY_WEST, menu_anchor = GDK_GRAVITY_EAST; break;
    default:         anchor = GDK_GRAVITY_NORTH, menu_anchor = GDK_GRAVITY_SOUTH; break;
    }
    gtk_menu_popup_at_rect(GTK_MENU(d->menu), gtk_widget_get_window(d->window), &rect, anchor, menu_anchor, event);
    gtk_widget_queue_draw(d->area);
}

/* ---- input --------------------------------------------------------------- */

/* ---- drag and drop ------------------------------------------------------- */

static void set_cursor(Dock *d, const char *name)
{
    GdkWindow *window = gtk_widget_get_window(d->area);
    g_autoptr(GdkCursor) cursor = name ? gdk_cursor_new_from_name(gdk_window_get_display(window), name) : NULL;
    gdk_window_set_cursor(window, cursor);
}

/* Work out where the dragged icon would land, from the current layout: the
 * icons it has passed, and which side of the separator it is on. Because the
 * other icons move as the gap moves, the gap only swaps with a neighbour once
 * the dragged icon's centre passes that neighbour's centre, so it never
 * flickers between two places. */
static void update_drop_target(Dock *d)
{
    double x, y;
    drag_icon_origin(d, &x, &y);
    double center = along(d, x + d->size / 2, y + d->size / 2);

    guint before_pinned = 0, before_running = 0, n_pinned = 0;
    gboolean has_separator = FALSE;
    double separator_center = 0;
    for (guint i = 0; i < d->slots->len; i++) {
        const Slot *sl = &g_array_index(d->slots, Slot, i);
        double c = sl->pos + sl->len / 2;
        if (sl->kind == SLOT_SEPARATOR) {
            has_separator = TRUE;
            separator_center = c;
        } else if (sl->kind == SLOT_ICON) {
            if (sl->item->pinned)
                n_pinned++, before_pinned += c < center;
            else
                before_running += c < center;
        }
    }

    DockItem *item = d->drag_item;
    gboolean to_pinned;
    if (item->pinned)
        to_pinned = TRUE; /* pinned icons reorder within the pinned group */
    else if (!item->app_info)
        to_pinned = FALSE; /* no .desktop file: can't be pinned */
    else if (has_separator)
        to_pinned = center < separator_center;
    else
        to_pinned = before_pinned < n_pinned; /* past the last pinned icon: stays unpinned */

    guint index = to_pinned ? before_pinned : before_running;
    if (index != d->drop_index || to_pinned != d->drop_pinned) {
        d->drop_index = index;
        d->drop_pinned = to_pinned;
    }
}

static void start_drag(Dock *d)
{
    DockItem *item = d->pressed;
    GPtrArray *items = app_tracker_items(d->tracker);
    guint index = 0; /* the item's current place in its group: where it lands if dropped now */
    for (guint i = 0; i < items->len; i++) {
        DockItem *it = g_ptr_array_index(items, i);
        if (it == item)
            break;
        index += it->pinned == item->pinned;
    }
    d->drag_item = item;
    d->drop_index = index;
    d->drop_pinned = item->pinned;
    d->pressed = NULL;
    d->hovered = NULL;
    d->zoom_target = 0; /* keep the row still while rearranging it */
    set_cursor(d, "grabbing");
}

static void end_drag(Dock *d)
{
    d->drag_item = NULL;
    set_cursor(d, NULL);
}

/* ---- input --------------------------------------------------------------- */

static gboolean on_motion(GtkWidget *widget, GdkEvent *event, Dock *d)
{
    double x, y;
    gdk_event_get_coords(event, &x, &y);
    if (d->pressed && !d->drag_item && hypot(x - d->press_x, y - d->press_y) > DRAG_THRESHOLD)
        start_drag(d);
    if (d->drag_item) {
        d->drag_x = x;
        d->drag_y = y;
        update_drop_target(d);
        refresh(d);
        animate(d);
        return FALSE;
    }

    d->mouse_pos = along(d, x, y);
    d->has_mouse_pos = TRUE;
    d->zoom_target = 1;
    d->pointer_inside = TRUE;
    show_dock(d);
    Slot *sl = slot_at(d, d->mouse_pos);
    d->hovered = sl ? sl->item : NULL;
    refresh(d);
    animate(d);
    return FALSE;
}

static gboolean on_leave(GtkWidget *widget, GdkEventCrossing *event, Dock *d)
{
    /* While a button is held the dock keeps the pointer (an implicit grab), so
     * leaving is handled on release instead. */
    if (menu_open(d) || d->pressed || d->drag_item)
        return FALSE;
    pointer_left(d);
    return FALSE;
}

static gboolean on_press(GtkWidget *widget, GdkEventButton *event, Dock *d)
{
    if (event->type != GDK_BUTTON_PRESS) /* ignore double/triple-click events */
        return FALSE;
    Slot *sl = slot_at(d, along(d, event->x, event->y));
    if (event->button == GDK_BUTTON_SECONDARY) {
        show_menu(d, sl, (GdkEvent *)event);
    } else if (sl) {
        Rect r = icon_rect(d, sl);
        d->pressed = sl->item;
        d->press_x = event->x;
        d->press_y = event->y;
        d->grab_fx = CLAMP((event->x - r.x) / r.w, 0, 1);
        d->grab_fy = CLAMP((event->y - r.y) / r.h, 0, 1);
        gtk_widget_queue_draw(d->area);
    }
    return TRUE;
}

static gboolean on_release(GtkWidget *widget, GdkEventButton *event, Dock *d)
{
    if (d->drag_item) {
        DockItem *item = d->drag_item;
        end_drag(d);
        app_tracker_move_item(d->tracker, item, d->drop_index, d->drop_pinned);
    } else {
        DockItem *pressed = d->pressed;
        d->pressed = NULL;
        Slot *sl = slot_at(d, along(d, event->x, event->y));
        if (pressed && sl && sl->item == pressed && over_dock(d, event->x, event->y)) {
            if (event->button == GDK_BUTTON_PRIMARY)
                app_tracker_activate(d->tracker, pressed, event->time);
            else if (event->button == GDK_BUTTON_MIDDLE)
                app_tracker_launch(d->tracker, pressed, event->time);
        }
    }
    refresh(d);
    if (!over_dock(d, event->x, event->y))
        pointer_left(d);
    return TRUE;
}

/* ---- model updates ------------------------------------------------------- */

static void on_items_changed(gpointer data)
{
    Dock *d = data;
    if (d->prefs)
        preferences_sync(d->prefs);
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
    if (d->drag_item == item) /* the app closed while being dragged */
        end_drag(d);
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

/* ---- settings ------------------------------------------------------------ */

static const Theme *theme_for(const DockConfig *cfg)
{
    return g_strcmp0(cfg->theme, "light") == 0 ? &theme_light : &theme_dark;
}

/* The config changed (from the settings window): rebuild everything that
 * depends on it, live. */
static void apply_config(gpointer data)
{
    Dock *d = data;
    d->colors = theme_for(d->cfg);
    d->has_mouse_pos = FALSE; /* the dock may have moved to another edge */
    if (!d->cfg->autohide)
        show_dock(d);
    else if (!d->pointer_inside && d->hide_target == 0 && !d->hide_timeout_id)
        schedule_hide(d);
    GPtrArray *items = app_tracker_items(d->tracker);
    for (guint i = 0; i < items->len; i++)
        dock_item_clear_icons(g_ptr_array_index(items, i)); /* icon size may have changed */
    memset(&d->input_rect, 0, sizeof d->input_rect);         /* force a new input shape */
    place(d);
    refresh(d);
}

static void on_preferences_closed(gpointer data)
{
    ((Dock *)data)->prefs = NULL;
}

void dock_show_preferences(Dock *d, guint32 timestamp)
{
    if (!d->prefs) {
        PreferencesCallbacks callbacks = { apply_config, on_preferences_closed, d };
        d->prefs = preferences_new(d->app, d->cfg, d->tracker, &callbacks);
    }
    preferences_present(d->prefs, timestamp);
}

/* ---- construction -------------------------------------------------------- */

Dock *dock_new(GtkApplication *app, DockConfig *cfg)
{
    Dock *d = g_new0(Dock, 1);
    d->cfg = cfg;
    d->app = app;
    d->colors = theme_for(cfg);
    d->slots = g_array_new(FALSE, TRUE, sizeof(Slot));
    d->hide = d->hide_target = cfg->autohide ? 1 : 0;

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
