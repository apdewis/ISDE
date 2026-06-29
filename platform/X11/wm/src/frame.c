#define _POSIX_C_SOURCE 200809L
/*
 * frame.c — window frame creation using raw XCB + Cairo
 *
 * Each frame is an override-redirect window containing:
 *   - Title bar drawn with Cairo (icon, title text, min/max/close buttons)
 *   - Input-only child windows for button click detection
 *   - The client window reparented below the title area
 */
#include "wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb_cursor.h>

#define GRIP_SIZE 6

/* Allocate an X pixel for a 0xRRGGBB colour from the default colormap. */
static uint32_t frame_pixel(Wm *wm, unsigned int rgb)
{
    xcb_alloc_color_reply_t *r = xcb_alloc_color_reply(
        wm->conn,
        xcb_alloc_color(wm->conn, wm->screen->default_colormap,
                        ((rgb >> 16) & 0xFF) * 257,
                        ((rgb >> 8)  & 0xFF) * 257,
                        ( rgb        & 0xFF) * 257),
        NULL);
    if (!r) {
        return wm->screen->black_pixel;
    }
    uint32_t px = r->pixel;
    free(r);
    return px;
}

/* ---------- icon loading ---------- */

void frame_init_icons(Wm *wm)
{
    int icon_sz = wm->title_height - wm_scale(wm, 4);
    if (icon_sz < 8) { icon_sz = 8; }

    cairo_surface_t **all[] = {
        &wm->icon_minimize, &wm->icon_maximize, &wm->icon_restore,
        &wm->icon_close, &wm->icon_menu,
        &wm->icon_minimize_inv, &wm->icon_maximize_inv, &wm->icon_restore_inv,
        &wm->icon_close_inv, &wm->icon_menu_inv,
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (*all[i]) { cairo_surface_destroy(*all[i]); *all[i] = NULL; }
    }

    /* Icons use SVG `currentColor`.  Normal icons take the button foreground;
     * the inverted (pressed) icons take the button background, so a pressed
     * button is the same colours swapped. */
    const IsdeColorScheme *s = isde_theme_current();
    unsigned int fg       = s ? s->titlebar_button.fg : 0x000000;
    unsigned int bg       = s ? s->titlebar_button.bg : 0xFFFFFF;
    unsigned int close_fg = s ? s->close_button.fg : 0x000000;
    unsigned int close_bg = s ? s->close_button.bg : 0xFFFFFF;

    struct { const char *name; cairo_surface_t **dst; unsigned int tint; } icons[] = {
        { "window-minimize", &wm->icon_minimize,     fg },
        { "window-maximize", &wm->icon_maximize,     fg },
        { "window-restore",  &wm->icon_restore,      fg },
        { "window-close",    &wm->icon_close,        close_fg },
        { "application-menu",&wm->icon_menu,         fg },
        { "window-minimize", &wm->icon_minimize_inv, bg },
        { "window-maximize", &wm->icon_maximize_inv, bg },
        { "window-restore",  &wm->icon_restore_inv,  bg },
        { "window-close",    &wm->icon_close_inv,    close_bg },
        { "application-menu",&wm->icon_menu_inv,     bg },
    };
    for (size_t i = 0; i < sizeof(icons) / sizeof(icons[0]); i++) {
        char *path = isde_icon_find("actions", icons[i].name);
        *icons[i].dst = path
            ? render_svg_to_surface(path, icon_sz, icons[i].tint) : NULL;
        free(path);
    }
}

/* ---------- title fetching ---------- */

static char *fetch_title(Wm *wm, xcb_window_t win)
{
    xcb_get_property_reply_t *reply = xcb_get_property_reply(
        wm->conn,
        xcb_get_property(wm->conn, 0, win, wm->atom_net_wm_name,
                         XCB_ATOM_ANY, 0, 256),
        NULL);
    if (reply && reply->value_len > 0) {
        char *title = strndup(xcb_get_property_value(reply),
                              reply->value_len);
        free(reply);
        return title;
    }
    free(reply);

    reply = xcb_get_property_reply(
        wm->conn,
        xcb_get_property(wm->conn, 0, win, wm->atom_wm_name,
                         XCB_ATOM_ANY, 0, 256),
        NULL);
    if (reply && reply->value_len > 0) {
        char *title = strndup(xcb_get_property_value(reply),
                              reply->value_len);
        free(reply);
        return title;
    }
    free(reply);

    return strdup("(untitled)");
}

static char *fetch_icon_name(Wm *wm, xcb_window_t win)
{
    xcb_get_property_reply_t *reply = xcb_get_property_reply(
        wm->conn,
        xcb_get_property(wm->conn, 0, win, wm->atom_net_wm_icon_name,
                         XCB_ATOM_ANY, 0, 256),
        NULL);
    if (reply && reply->value_len > 0) {
        char *name = strndup(xcb_get_property_value(reply),
                             reply->value_len);
        free(reply);
        return name;
    }
    free(reply);

    reply = xcb_get_property_reply(
        wm->conn,
        xcb_get_property(wm->conn, 0, win, wm->atom_wm_icon_name,
                         XCB_ATOM_ANY, 0, 256),
        NULL);
    if (reply && reply->value_len > 0) {
        char *name = strndup(xcb_get_property_value(reply),
                             reply->value_len);
        free(reply);
        return name;
    }
    free(reply);

    return NULL;
}

/* ---------- disambiguation ---------- */

static void disambiguate_name(Wm *wm, WmClient *target,
                              const char *raw, int is_icon)
{
    xcb_ewmh_connection_t *ewmh = isde_ewmh_connection(wm->ewmh);
    int suffix = 1;

    for (WmClient *c = wm->clients; c; c = c->next) {
        if (c == target) {
            break;
        }
        const char *other = is_icon ? c->icon_name : c->title;
        if (other && strcmp(other, raw) == 0) {
            suffix++;
        }
    }

    if (suffix == 1) {
        if (is_icon) {
            free(target->visible_icon_name);
            target->visible_icon_name = NULL;
            xcb_delete_property(wm->conn, target->client,
                                ewmh->_NET_WM_VISIBLE_ICON_NAME);
        } else {
            free(target->visible_name);
            target->visible_name = NULL;
            xcb_delete_property(wm->conn, target->client,
                                ewmh->_NET_WM_VISIBLE_NAME);
        }
        return;
    }

    char buf[512];
    snprintf(buf, sizeof(buf), "%s [%d]", raw, suffix);

    if (is_icon) {
        free(target->visible_icon_name);
        target->visible_icon_name = strdup(buf);
        xcb_ewmh_set_wm_visible_icon_name(ewmh, target->client,
                                           strlen(buf), buf);
    } else {
        free(target->visible_name);
        target->visible_name = strdup(buf);
        xcb_ewmh_set_wm_visible_name(ewmh, target->client,
                                      strlen(buf), buf);
    }
}

void frame_disambiguate_all(Wm *wm, const char *base_title,
                            const char *base_icon)
{
    for (WmClient *c = wm->clients; c; c = c->next) {
        if (base_title && c->title && strcmp(c->title, base_title) == 0) {
            disambiguate_name(wm, c, c->title, 0);
        }
        if (base_icon && c->icon_name &&
            strcmp(c->icon_name, base_icon) == 0) {
            disambiguate_name(wm, c, c->icon_name, 1);
        }
    }
}

/* ---------- size hints ---------- */

void frame_read_size_hints(Wm *wm, WmClient *c)
{
    xcb_size_hints_t hints;

    c->min_w = 0;  c->min_h = 0;
    c->max_w = 0;  c->max_h = 0;
    c->base_w = 0; c->base_h = 0;
    c->inc_w = 1;  c->inc_h = 1;
    c->fixed_size = 0;
    c->win_gravity = XCB_GRAVITY_NORTH_WEST;

    if (!xcb_icccm_get_wm_normal_hints_reply(
            wm->conn,
            xcb_icccm_get_wm_normal_hints(wm->conn, c->client),
            &hints, NULL)) {
        return;
    }

    if (hints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) {
        c->min_w = hints.min_width;
        c->min_h = hints.min_height;
    }
    if (hints.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) {
        c->max_w = hints.max_width;
        c->max_h = hints.max_height;
    }
    if (hints.flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE) {
        c->base_w = hints.base_width;
        c->base_h = hints.base_height;
    }
    if (hints.flags & XCB_ICCCM_SIZE_HINT_P_RESIZE_INC) {
        c->inc_w = hints.width_inc;
        c->inc_h = hints.height_inc;
        if (c->inc_w < 1) { c->inc_w = 1; }
        if (c->inc_h < 1) { c->inc_h = 1; }
    }
    if (hints.flags & XCB_ICCCM_SIZE_HINT_P_WIN_GRAVITY) {
        c->win_gravity = hints.win_gravity;
    }

    c->fixed_size = (c->min_w > 0 && c->max_w > 0 && c->min_w == c->max_w &&
                     c->min_h > 0 && c->max_h > 0 && c->min_h == c->max_h);
}

/* ---------- ICCCM size constraints (§4.1.2.3) ---------- */

void frame_constrain_size(WmClient *c, int *w, int *h)
{
    int bw = c->base_w > 0 ? c->base_w : (c->min_w > 0 ? c->min_w : 0);
    int bh = c->base_h > 0 ? c->base_h : (c->min_h > 0 ? c->min_h : 0);

    if (c->min_w > 0 && *w < c->min_w) { *w = c->min_w; }
    if (c->min_h > 0 && *h < c->min_h) { *h = c->min_h; }
    if (c->max_w > 0 && *w > c->max_w) { *w = c->max_w; }
    if (c->max_h > 0 && *h > c->max_h) { *h = c->max_h; }

    if (c->inc_w > 1) {
        *w = bw + ((*w - bw) / c->inc_w) * c->inc_w;
    }
    if (c->inc_h > 1) {
        *h = bh + ((*h - bh) / c->inc_h) * c->inc_h;
    }
}

/* Convert client-supplied (gravity-relative) coordinates to frame position.
 * ICCCM §4.1.2.3: the client's x/y in a ConfigureRequest specify where
 * the reference point (defined by win_gravity) of the client window should
 * appear on root. The WM must convert to frame content-origin coordinates.
 *
 * The client window sits inside the frame at root-absolute position:
 *   abs_x = frame_x + frame_bw + WM_BORDER_WIDTH
 *   abs_y = frame_y + frame_bw + WM_BORDER_WIDTH + title
 * where frame_bw is the X border on the frame window (1px). */
void frame_gravity_to_frame(Wm *wm, WmClient *c,
                            int client_x, int client_y,
                            int *frame_x, int *frame_y)
{
    int title = c->decorated ? wm->title_height : 0;
    int bw = 1;
    int left = WM_BORDER_WIDTH + bw;
    int top  = WM_BORDER_WIDTH + title + bw;
    int cw   = c->width;
    int ch   = c->height;

    switch (c->win_gravity) {
    default:
    case XCB_GRAVITY_NORTH_WEST:
        *frame_x = client_x - left;
        *frame_y = client_y - top;
        break;
    case XCB_GRAVITY_NORTH:
        *frame_x = client_x - left - cw / 2;
        *frame_y = client_y - top;
        break;
    case XCB_GRAVITY_NORTH_EAST:
        *frame_x = client_x - left - cw;
        *frame_y = client_y - top;
        break;
    case XCB_GRAVITY_WEST:
        *frame_x = client_x - left;
        *frame_y = client_y - top - ch / 2;
        break;
    case XCB_GRAVITY_CENTER:
        *frame_x = client_x - left - cw / 2;
        *frame_y = client_y - top - ch / 2;
        break;
    case XCB_GRAVITY_EAST:
        *frame_x = client_x - left - cw;
        *frame_y = client_y - top - ch / 2;
        break;
    case XCB_GRAVITY_SOUTH_WEST:
        *frame_x = client_x - left;
        *frame_y = client_y - top - ch;
        break;
    case XCB_GRAVITY_SOUTH:
        *frame_x = client_x - left - cw / 2;
        *frame_y = client_y - top - ch;
        break;
    case XCB_GRAVITY_SOUTH_EAST:
        *frame_x = client_x - left - cw;
        *frame_y = client_y - top - ch;
        break;
    case XCB_GRAVITY_STATIC:
        *frame_x = client_x - left;
        *frame_y = client_y - top;
        break;
    }
}

/* Inverse: convert frame position back to the gravity-relative position
 * the client originally specified (used to fill in the unchanged axis
 * when a ConfigureRequest only sets x or only y). */
void frame_frame_to_gravity(Wm *wm, WmClient *c,
                            int *client_x, int *client_y)
{
    int title = c->decorated ? wm->title_height : 0;
    int bw = 1;
    int left = WM_BORDER_WIDTH + bw;
    int top  = WM_BORDER_WIDTH + title + bw;
    int cw   = c->width;
    int ch   = c->height;

    switch (c->win_gravity) {
    default:
    case XCB_GRAVITY_NORTH_WEST:
        *client_x = c->x + left;
        *client_y = c->y + top;
        break;
    case XCB_GRAVITY_NORTH:
        *client_x = c->x + left + cw / 2;
        *client_y = c->y + top;
        break;
    case XCB_GRAVITY_NORTH_EAST:
        *client_x = c->x + left + cw;
        *client_y = c->y + top;
        break;
    case XCB_GRAVITY_WEST:
        *client_x = c->x + left;
        *client_y = c->y + top + ch / 2;
        break;
    case XCB_GRAVITY_CENTER:
        *client_x = c->x + left + cw / 2;
        *client_y = c->y + top + ch / 2;
        break;
    case XCB_GRAVITY_EAST:
        *client_x = c->x + left + cw;
        *client_y = c->y + top + ch / 2;
        break;
    case XCB_GRAVITY_SOUTH_WEST:
        *client_x = c->x + left;
        *client_y = c->y + top + ch;
        break;
    case XCB_GRAVITY_SOUTH:
        *client_x = c->x + left + cw / 2;
        *client_y = c->y + top + ch;
        break;
    case XCB_GRAVITY_SOUTH_EAST:
        *client_x = c->x + left + cw;
        *client_y = c->y + top + ch;
        break;
    case XCB_GRAVITY_STATIC:
        *client_x = c->x + left;
        *client_y = c->y + top;
        break;
    }
}

/* ---------- title bar layout ----------
 * Buttons are hit-tested by coordinate on the frame window; there are no
 * separate sub-windows.  Layout (frame-relative x), th = title height:
 *   menu     [0,       th)
 *   title    [th,      fw-3th)
 *   minimize [fw-3th,  fw-2th)
 *   maximize [fw-2th,  fw-th)
 *   close    [fw-th,   fw)
 */

int frame_button_at(Wm *wm, WmClient *c, int x, int y)
{
    if (!c->decorated || y < 0 || y >= wm->title_height) {
        return -1;
    }
    int th = wm->title_height;
    int fw = frame_total_width(c);

    if (x >= 0 && x < th) {
        return FRAME_BTN_MENU;
    }
    if (x >= fw - 3 * th && x < fw - 2 * th) {
        return FRAME_BTN_MINIMIZE;
    }
    if (x >= fw - 2 * th && x < fw - th) {
        return FRAME_BTN_MAXIMIZE;
    }
    if (x >= fw - th && x < fw) {
        return FRAME_BTN_CLOSE;
    }
    return -1;
}

/* ---------- frame painting ---------- */

static void alloc_frame_surface(Wm *wm, WmClient *c)
{
    if (c->frame_surface) {
        cairo_surface_destroy(c->frame_surface);
    }
    int fw = frame_total_width(c);
    int fh = frame_total_height(wm, c);
    c->frame_surface = render_surface_for_window(wm->conn, wm->screen,
                                                  c->frame, fw, fh);
}

void frame_paint(Wm *wm, WmClient *c)
{
    if (!c->decorated || !c->frame_surface) {
        return;
    }

    const IsdeColorScheme *s = isde_theme_current();
    if (!s) {
        return;
    }

    const IsdeElementColors *tb = c->focused
        ? &s->titlebar_active : &s->titlebar;

    int th = wm->title_height;
    int fw = frame_total_width(c);
    int title_x = th;
    int title_w = fw - 4 * th;   /* menu + 3 right-hand buttons */
    if (title_w < 1) { title_w = 1; }

    cairo_t *cr = cairo_create(c->frame_surface);

    const IsdeElementColors *btn_colors = &s->titlebar_button;
    const IsdeElementColors *close_colors = &s->close_button;

    int min_x   = fw - 3 * th;
    int max_x   = fw - 2 * th;
    int close_x = fw - th;

    /* Only the title label region reflects focus (active vs inactive);
     * the buttons keep their own background regardless of focus. */
    render_fill_rect(cr, tb->bg, title_x, 0, title_w, th);

    /* Menu button (left) — styled like the other buttons */
    render_fill_rect(cr, btn_colors->bg, 0, 0, th, th);
    render_icon(cr, wm->icon_menu, 0, 0, th, th);

    /* Title text */
    const char *display = c->visible_name ? c->visible_name
                        : c->title ? c->title : "(untitled)";
    int font_px = th - wm_scale(wm, 8);
    if (font_px < 8) { font_px = 8; }
    render_text_centered(cr, display, tb->fg, title_x, 0, title_w, th, font_px);

    /* Right-hand buttons — must match frame_button_at() layout */
    render_fill_rect(cr, btn_colors->bg, min_x, 0, th, th);
    render_icon(cr, wm->icon_minimize, min_x, 0, th, th);

    render_fill_rect(cr, btn_colors->bg, max_x, 0, th, th);
    render_icon(cr, c->maximized ? wm->icon_restore : wm->icon_maximize,
                max_x, 0, th, th);

    render_fill_rect(cr, close_colors->bg, close_x, 0, th, th);
    render_icon(cr, wm->icon_close, close_x, 0, th, th);

    /* The button currently held down is drawn with bg and fg swapped:
     * fill with the foreground colour and draw the background-tinted icon. */
    if (wm->btn_press_client == c && wm->btn_press_hover &&
        wm->btn_press_btn >= 0) {
        int bx = 0;
        unsigned int swap_bg = btn_colors->fg;
        cairo_surface_t *inv = NULL;
        switch (wm->btn_press_btn) {
        case FRAME_BTN_MENU:
            bx = 0; swap_bg = btn_colors->fg; inv = wm->icon_menu_inv;
            break;
        case FRAME_BTN_MINIMIZE:
            bx = min_x; swap_bg = btn_colors->fg; inv = wm->icon_minimize_inv;
            break;
        case FRAME_BTN_MAXIMIZE:
            bx = max_x; swap_bg = btn_colors->fg;
            inv = c->maximized ? wm->icon_restore_inv : wm->icon_maximize_inv;
            break;
        case FRAME_BTN_CLOSE:
            bx = close_x; swap_bg = close_colors->fg; inv = wm->icon_close_inv;
            break;
        }
        render_fill_rect(cr, swap_bg, bx, 0, th, th);
        render_icon(cr, inv, bx, 0, th, th);
    }

    cairo_destroy(cr);
    cairo_surface_flush(c->frame_surface);
    xcb_flush(wm->conn);
}

void frame_apply_theme(Wm *wm, WmClient *c)
{
    if (!c->decorated) {
        return;
    }
    frame_paint(wm, c);
}

/* Re-read the active colour scheme into an existing frame after a live
 * appearance change: refresh the X border pixel and repaint the title bar. */
void frame_refresh_theme(Wm *wm, WmClient *c)
{
    const IsdeColorScheme *scheme = isde_theme_current();
    if (scheme) {
        uint32_t border_pixel = frame_pixel(wm, scheme->titlebar.border);
        xcb_change_window_attributes(wm->conn, c->frame,
                                     XCB_CW_BORDER_PIXEL, &border_pixel);
    }
    if (c->decorated) {
        frame_paint(wm, c);
    }
}

/* ---------- frame creation ---------- */

WmClient *frame_create(Wm *wm, xcb_window_t client, int adopt)
{
    xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(
        wm->conn, xcb_get_geometry(wm->conn, client), NULL);
    if (!geo) {
        return NULL;
    }

    WmClient *c = calloc(1, sizeof(*c));
    if (!c) { free(geo); return NULL; }

    c->client = client;
    c->x      = geo->x;
    c->y      = geo->y;
    c->width  = geo->width;
    c->height = geo->height;
    free(geo);

    frame_read_size_hints(wm, c);

    c->input_hint = 1;
    c->initial_state = 1;
    xcb_icccm_wm_hints_t wm_hints;
    if (xcb_icccm_get_wm_hints_reply(wm->conn,
            xcb_icccm_get_wm_hints(wm->conn, client),
            &wm_hints, NULL)) {
        if (wm_hints.flags & XCB_ICCCM_WM_HINT_INPUT) {
            c->input_hint = wm_hints.input;
        }
        if (wm_hints.flags & XCB_ICCCM_WM_HINT_STATE) {
            c->initial_state = wm_hints.initial_state;
        }
    }

    c->decorated = wm_client_wants_decorations(wm, client) &&
                   wm_window_type_wants_decorations(wm, client);

    c->title = fetch_title(wm, client);

    /* Read initial _NET_WM_STATE */
    xcb_ewmh_get_atoms_reply_t init_state;
    if (xcb_ewmh_get_wm_state_reply(
            isde_ewmh_connection(wm->ewmh),
            xcb_ewmh_get_wm_state(isde_ewmh_connection(wm->ewmh), client),
            &init_state, NULL)) {
        xcb_ewmh_connection_t *ec = isde_ewmh_connection(wm->ewmh);
        for (uint32_t s = 0; s < init_state.atoms_len; s++) {
            if (init_state.atoms[s] == ec->_NET_WM_STATE_FULLSCREEN)
                c->fullscreen = 1;
            else if (init_state.atoms[s] == ec->_NET_WM_STATE_ABOVE)
                c->above = 1;
            else if (init_state.atoms[s] == ec->_NET_WM_STATE_BELOW)
                c->below = 1;
            else if (init_state.atoms[s] == ec->_NET_WM_STATE_MODAL)
                c->modal = 1;
            else if (init_state.atoms[s] == ec->_NET_WM_STATE_STICKY)
                c->sticky = 1;
            else if (init_state.atoms[s] == ec->_NET_WM_STATE_SKIP_TASKBAR)
                c->skip_taskbar = 1;
            else if (init_state.atoms[s] == ec->_NET_WM_STATE_SKIP_PAGER)
                c->skip_pager = 1;
            else if (init_state.atoms[s] == ec->_NET_WM_STATE_DEMANDS_ATTENTION)
                c->demands_attention = 1;
        }
        xcb_ewmh_get_atoms_reply_wipe(&init_state);
    }

    xcb_get_property_reply_t *utw_reply = xcb_get_property_reply(wm->conn,
        xcb_get_property(wm->conn, 0, client,
                         wm->atom_net_wm_user_time_window,
                         XCB_ATOM_WINDOW, 0, 1), NULL);
    if (utw_reply && xcb_get_property_value_length(utw_reply) >= 4) {
        c->user_time_window = *(xcb_window_t *)xcb_get_property_value(utw_reply);
        uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
        xcb_change_window_attributes(wm->conn, c->user_time_window,
                                     XCB_CW_EVENT_MASK, &mask);
    }
    free(utw_reply);

    xcb_window_t ut_win = c->user_time_window ? c->user_time_window : client;
    xcb_get_property_reply_t *ut_reply = xcb_get_property_reply(wm->conn,
        xcb_get_property(wm->conn, 0, ut_win,
                         wm->atom_net_wm_user_time,
                         XCB_ATOM_CARDINAL, 0, 1), NULL);
    if (ut_reply && xcb_get_property_value_length(ut_reply) >= 4) {
        c->user_time = *(uint32_t *)xcb_get_property_value(ut_reply);
    }
    free(ut_reply);

    xcb_get_property_reply_t *sid_reply = xcb_get_property_reply(wm->conn,
        xcb_get_property(wm->conn, 0, client,
                         wm->atom_net_startup_id,
                         XCB_ATOM_ANY, 0, 256), NULL);
    if (sid_reply && sid_reply->value_len > 0) {
        c->startup_id = strndup(xcb_get_property_value(sid_reply),
                                sid_reply->value_len);
    }
    free(sid_reply);

    if (adopt) {
        int title = c->decorated ? wm->title_height : 0;
        int bw = 1;
        c->x -= WM_BORDER_WIDTH + bw;
        c->y -= WM_BORDER_WIDTH + title + bw;
        xcb_icccm_get_wm_transient_for_reply(
            wm->conn,
            xcb_icccm_get_wm_transient_for(wm->conn, c->client),
            &c->transient_for, NULL);
    } else {
        wm_place_client(wm, c);
    }

    int fw = frame_total_width(c);
    int fh = frame_total_height(wm, c);

    /* Allocate border pixel color */
    const IsdeColorScheme *scheme = isde_theme_current();
    uint32_t border_pixel = scheme
        ? frame_pixel(wm, scheme->titlebar.border)
        : wm->screen->black_pixel;

    /* Create override-redirect frame window */
    c->frame = xcb_generate_id(wm->conn);
    uint32_t vals[] = {
        wm->screen->black_pixel,
        border_pixel,
        1,
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_EXPOSURE |
        XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
    };
    xcb_create_window(wm->conn, XCB_COPY_FROM_PARENT,
                      c->frame, wm->root,
                      c->x, c->y, fw, fh, 1,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      wm->screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
                      XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK,
                      vals);

    /* Create Cairo surface for frame painting */
    frame_init_cursors(wm);
    if (c->decorated) {
        alloc_frame_surface(wm, c);
    }

    /* Reparent client into frame */
    xcb_change_save_set(wm->conn, XCB_SET_MODE_INSERT, client);

    {
        int title = c->decorated ? wm->title_height : 0;
        xcb_reparent_window(wm->conn, client, c->frame,
                            WM_BORDER_WIDTH, WM_BORDER_WIDTH + title);
    }

    /* Create resize grips */
    if (c->decorated && !c->fixed_size) {
        frame_create_grips(wm, c);
    }

    /* Remove client border and sync client to (possibly clamped) size */
    uint32_t init_cfg[] = { (uint32_t)c->width, (uint32_t)c->height, 0 };
    xcb_configure_window(wm->conn, client,
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                         XCB_CONFIG_WINDOW_BORDER_WIDTH, init_cfg);

    /* Listen for property changes on the client */
    uint32_t client_mask = XCB_EVENT_MASK_PROPERTY_CHANGE |
                           XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(wm->conn, client,
                                 XCB_CW_EVENT_MASK, &client_mask);

    /* Passive grab for click-to-focus */
    xcb_grab_button(wm->conn, 0, client,
                    XCB_EVENT_MASK_BUTTON_PRESS,
                    XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                    XCB_NONE, XCB_NONE,
                    XCB_BUTTON_INDEX_1, XCB_MOD_MASK_ANY);

    frame_set_extents(wm, c);

    /* Initial paint */
    frame_paint(wm, c);

    /* Append to list tail */
    c->next = NULL;
    if (!wm->clients) {
        wm->clients = c;
    } else {
        WmClient *tail = wm->clients;
        while (tail->next) {
            tail = tail->next;
        }
        tail->next = c;
    }

    return c;
}

/* ---------- frame destruction ---------- */

void frame_destroy(Wm *wm, WmClient *c)
{
    frame_destroy_grips(wm, c);

    xcb_change_save_set(wm->conn, XCB_SET_MODE_DELETE, c->client);
    int title = c->decorated ? wm->title_height : 0;
    int bw = 1;
    int cx = c->x + WM_BORDER_WIDTH + bw;
    int cy = c->y + WM_BORDER_WIDTH + title + bw;
    xcb_reparent_window(wm->conn, c->client, wm->root, cx, cy);
    if (!c->minimized) {
        xcb_map_window(wm->conn, c->client);
    }
    xcb_flush(wm->conn);

    if (c->frame_surface) {
        cairo_surface_destroy(c->frame_surface);
    }
    if (c->frame) {
        xcb_destroy_window(wm->conn, c->frame);
    }

    free(c->title);
    free(c->icon_name);
    free(c->visible_name);
    free(c->visible_icon_name);
    free(c->startup_id);
    free(c);
}

/* ---------- _NET_FRAME_EXTENTS ---------- */

void frame_set_extents(Wm *wm, WmClient *c)
{
    if (!c->decorated) {
        xcb_ewmh_set_frame_extents(isde_ewmh_connection(wm->ewmh),
                                   c->client, 0, 0, 0, 0);
        return;
    }
    int bw = c->maximized ? 0 : 1;
    int title = wm->title_height;
    uint32_t left   = WM_BORDER_WIDTH + bw;
    uint32_t right  = left;
    uint32_t top    = WM_BORDER_WIDTH + title + bw;
    uint32_t bottom = WM_BORDER_WIDTH + bw;
    xcb_ewmh_set_frame_extents(isde_ewmh_connection(wm->ewmh),
                               c->client, left, right, top, bottom);
}

/* ---------- synthetic ConfigureNotify (ICCCM §4.2.3) ---------- */

void frame_send_configure_notify(Wm *wm, WmClient *c)
{
    xcb_configure_notify_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.response_type = XCB_CONFIGURE_NOTIFY;
    ev.event = c->client;
    ev.window = c->client;
    int title = c->decorated ? wm->title_height : 0;
    int bw = c->maximized ? 0 : 1;
    ev.x = c->x + WM_BORDER_WIDTH + bw;
    ev.y = c->y + WM_BORDER_WIDTH + title + bw;
    ev.width = c->width;
    ev.height = c->height;
    ev.border_width = 0;
    ev.above_sibling = XCB_WINDOW_NONE;
    ev.override_redirect = 0;
    xcb_send_event(wm->conn, 0, c->client,
                   XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&ev);
}

/* ---------- frame geometry ---------- */

int frame_total_width(WmClient *c)
{
    return c->width + 2 * WM_BORDER_WIDTH;
}

int frame_total_height(Wm *wm, WmClient *c)
{
    int title = c->decorated ? wm->title_height : 0;
    return c->height + title + 2 * WM_BORDER_WIDTH;
}

/* ---------- reconfigure frame + client ---------- */

void frame_configure(Wm *wm, WmClient *c)
{
    int fw = frame_total_width(c);
    int fh = frame_total_height(wm, c);
    int th = wm->title_height;
    int title = c->decorated ? th : 0;
    int bw = c->maximized ? 0 : 1;

    uint32_t frame_vals[] = { c->x, c->y, fw, fh, bw };
    xcb_configure_window(wm->conn, c->frame,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                         XCB_CONFIG_WINDOW_BORDER_WIDTH, frame_vals);

    if (c->decorated) {
        /* Reallocate Cairo surface for new size; buttons are hit-tested
         * by coordinate, so there are no sub-windows to reposition. */
        alloc_frame_surface(wm, c);
    }

    /* Reposition client window within frame */
    uint32_t cpos[] = { WM_BORDER_WIDTH, WM_BORDER_WIDTH + title };
    xcb_configure_window(wm->conn, c->client,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                         cpos);

    /* Resize client window */
    uint32_t cvals[] = { (uint32_t)c->width, (uint32_t)c->height };
    xcb_configure_window(wm->conn, c->client,
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         cvals);

    if (c->grip[0]) {
        frame_update_grips(wm, c);
    }

    frame_set_extents(wm, c);
    frame_send_configure_notify(wm, c);
    frame_paint(wm, c);
    xcb_flush(wm->conn);
}

/* ---------- title update ---------- */

void frame_update_title(Wm *wm, WmClient *c)
{
    char *old_title = c->title;
    char *old_icon  = c->icon_name;

    c->title     = fetch_title(wm, c->client);
    c->icon_name = fetch_icon_name(wm, c->client);

    frame_disambiguate_all(wm, c->title, c->icon_name);

    if (old_title && strcmp(old_title, c->title) != 0) {
        frame_disambiguate_all(wm, old_title, NULL);
    }
    if (old_icon && (!c->icon_name || strcmp(old_icon, c->icon_name) != 0)) {
        frame_disambiguate_all(wm, NULL, old_icon);
    }

    free(old_title);
    free(old_icon);

    frame_paint(wm, c);
}

/* ---------- resize cursors ---------- */

void frame_init_cursors(Wm *wm)
{
    if (wm->cursors[0]) {
        return;
    }
    xcb_cursor_context_t *ctx;
    if (xcb_cursor_context_new(wm->conn, wm->screen, &ctx) < 0) {
        return;
    }
    static const char *names[8] = {
        "top_side", "bottom_side", "left_side", "right_side",
        "top_left_corner", "top_right_corner",
        "bottom_left_corner", "bottom_right_corner"
    };
    for (int i = 0; i < 8; i++) {
        wm->cursors[i] = xcb_cursor_load_cursor(ctx, names[i]);
    }
    xcb_cursor_context_free(ctx);
}

/* ---------- resize grips ---------- */

void frame_create_grips(Wm *wm, WmClient *c)
{
    uint32_t mask = XCB_EVENT_MASK_BUTTON_PRESS |
                    XCB_EVENT_MASK_ENTER_WINDOW |
                    XCB_EVENT_MASK_LEAVE_WINDOW;

    for (int i = 0; i < 8; i++) {
        uint32_t vals[2] = { mask, wm->cursors[i] };
        c->grip[i] = xcb_generate_id(wm->conn);
        xcb_create_window(wm->conn, 0, c->grip[i], c->frame,
                          0, 0, 1, 1, 0,
                          XCB_WINDOW_CLASS_INPUT_ONLY,
                          XCB_COPY_FROM_PARENT,
                          XCB_CW_EVENT_MASK | XCB_CW_CURSOR, vals);
        xcb_map_window(wm->conn, c->grip[i]);
    }
    frame_update_grips(wm, c);
}

void frame_update_grips(Wm *wm, WmClient *c)
{
    int fw = frame_total_width(c);
    int fh = frame_total_height(wm, c);
    int g = wm_scale(wm, GRIP_SIZE);
    int th = wm->title_height;

    int client_top = th;
    int client_h = fh - th;

    struct { int x, y, w, h; } r[8] = {
        { 0,      0,        0, 0 },                      /* top — disabled */
        { g,      fh - g,   fw - 2*g, g },               /* bottom */
        { 0,      client_top, g, client_h - g },          /* left */
        { fw - g, client_top, g, client_h - g },          /* right */
        { 0,      0,        0, 0 },                       /* tl — disabled */
        { 0,      0,        0, 0 },                       /* tr — disabled */
        { 0,      fh - g,   g, g },                       /* bottom-left */
        { fw - g, fh - g,   g, g },                       /* bottom-right */
    };

    for (int i = 0; i < 8; i++) {
        if (r[i].w < 1 || r[i].h < 1) {
            xcb_unmap_window(wm->conn, c->grip[i]);
            continue;
        }
        xcb_map_window(wm->conn, c->grip[i]);
        uint32_t vals[] = { r[i].x, r[i].y, r[i].w, r[i].h,
                            XCB_STACK_MODE_ABOVE };
        xcb_configure_window(wm->conn, c->grip[i],
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                             XCB_CONFIG_WINDOW_STACK_MODE, vals);
    }
}

void frame_destroy_grips(Wm *wm, WmClient *c)
{
    for (int i = 0; i < 8; i++) {
        if (c->grip[i]) {
            xcb_destroy_window(wm->conn, c->grip[i]);
            c->grip[i] = 0;
        }
    }
}

/* ---------- window menu ---------- */

typedef struct {
    int desktop;
    int is_above;
    int is_below;
    int is_sticky;
} MenuAction;

void wm_dismiss_menu(Wm *wm)
{
    if (wm->win_menu) {
        xcb_ungrab_keyboard(wm->conn, XCB_CURRENT_TIME);
        xcb_ungrab_pointer(wm->conn, XCB_CURRENT_TIME);
        xcb_destroy_window(wm->conn, wm->win_menu);
        wm->win_menu = 0;
        wm->menu_client = NULL;
        wm->menu_item_count = 0;
        xcb_flush(wm->conn);
    }
}

static void menu_paint(Wm *wm)
{
    WmClient *c = wm->menu_client;
    if (!c || !wm->win_menu) {
        return;
    }

    const IsdeColorScheme *s = isde_theme_current();
    if (!s) {
        return;
    }

    int item_h = wm->menu_item_height;
    int total_items = 2 + 1 + wm->num_desktops + 1;
    int menu_w = wm_scale(wm, 180);
    int menu_h = total_items * item_h;

    cairo_surface_t *surface = render_surface_for_window(
        wm->conn, wm->screen, wm->win_menu, menu_w, menu_h);
    if (!surface) {
        return;
    }
    cairo_t *cr = cairo_create(surface);

    render_fill_rect(cr, s->menu.bg, 0, 0, menu_w, menu_h);

    int font_px = item_h - wm_scale(wm, 6);
    if (font_px < 8) { font_px = 8; }
    int y = 0;

    /* Above */
    render_text(cr, c->above ? "* Keep Above" : "  Keep Above",
                s->menu_item.fg, 0, y, menu_w, item_h, font_px);
    y += item_h;

    /* Below */
    render_text(cr, c->below ? "* Keep Below" : "  Keep Below",
                s->menu_item.fg, 0, y, menu_w, item_h, font_px);
    y += item_h;

    /* Separator */
    render_fill_rect(cr, s->border, 4, y + item_h/2, menu_w - 8, 1);
    y += item_h;

    /* Desktop entries */
    for (int d = 0; d < wm->num_desktops; d++) {
        char label[32];
        if (c->desktop == (uint32_t)d) {
            snprintf(label, sizeof(label), "* Desktop %d", d + 1);
        } else {
            snprintf(label, sizeof(label), "  Desktop %d", d + 1);
        }
        render_text(cr, label, s->menu_item.fg, 0, y, menu_w, item_h, font_px);
        y += item_h;
    }

    /* All Desktops (sticky) */
    render_text(cr, c->desktop == 0xFFFFFFFF ? "* All Desktops"
                                              : "  All Desktops",
                s->menu_item.fg, 0, y, menu_w, item_h, font_px);

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
    xcb_flush(wm->conn);
}

void win_menu_show(Wm *wm, WmClient *c)
{
    wm_dismiss_menu(wm);

    const IsdeColorScheme *s = isde_theme_current();
    int item_h = wm_scale(wm, isde_font_height("menu", 6));
    int total_items = 2 + 1 + wm->num_desktops + 1;
    int menu_w = wm_scale(wm, 180);
    int menu_h = total_items * item_h;

    /* Position below the menu button */
    int mx = c->x + WM_BORDER_WIDTH;
    int my = c->y + wm->title_height + 1;

    wm->win_menu = xcb_generate_id(wm->conn);
    uint32_t bg_pixel = wm->screen->white_pixel;
    if (s) {
        xcb_alloc_color_reply_t *cr = xcb_alloc_color_reply(
            wm->conn,
            xcb_alloc_color(wm->conn, wm->screen->default_colormap,
                            ((s->menu.bg >> 16) & 0xFF) * 257,
                            ((s->menu.bg >> 8)  & 0xFF) * 257,
                            ( s->menu.bg        & 0xFF) * 257),
            NULL);
        if (cr) { bg_pixel = cr->pixel; free(cr); }
    }

    uint32_t vals[] = {
        bg_pixel,
        1,
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_EXPOSURE
    };
    xcb_create_window(wm->conn, XCB_COPY_FROM_PARENT,
                      wm->win_menu, wm->root,
                      mx, my, menu_w, menu_h, 1,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      wm->screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
                      XCB_CW_EVENT_MASK,
                      vals);
    xcb_map_window(wm->conn, wm->win_menu);

    xcb_grab_keyboard(wm->conn, 0, wm->win_menu,
                      XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC,
                      XCB_GRAB_MODE_ASYNC);
    xcb_grab_pointer(wm->conn, 0, wm->win_menu,
                     XCB_EVENT_MASK_BUTTON_PRESS |
                     XCB_EVENT_MASK_BUTTON_RELEASE,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                     XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
    xcb_flush(wm->conn);

    wm->menu_client = c;
    wm->menu_item_count = total_items;
    wm->menu_item_height = item_h;

    menu_paint(wm);
}

int win_menu_click(Wm *wm, int16_t x, int16_t y)
{
    (void)x;
    WmClient *c = wm->menu_client;
    if (!c) {
        return 0;
    }

    int item_h = wm->menu_item_height;
    int idx = y / item_h;

    if (idx == 0) {
        wm_set_above(wm, c, !c->above);
    } else if (idx == 1) {
        wm_set_below(wm, c, !c->below);
    } else if (idx == 2) {
        /* separator — do nothing */
        wm_dismiss_menu(wm);
        return 1;
    } else if (idx >= 3 && idx < 3 + wm->num_desktops) {
        wm_move_to_desktop(wm, c, idx - 3);
    } else if (idx == 3 + wm->num_desktops) {
        wm_move_to_desktop(wm, c, 0xFFFFFFFF);
    }

    wm_dismiss_menu(wm);
    return 1;
}
