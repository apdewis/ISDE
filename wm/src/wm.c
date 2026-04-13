#define _POSIX_C_SOURCE 200809L
/*
 * wm.c — core window manager: Xt initialization, event loop, client management
 */
#include "wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <xcb/xcb_aux.h>
#include <isde/isde-theme.h>
#include <xcb/xcb_cursor.h>
#include <ISW/ISWRender.h>
#include "isde/isde-config.h"

/* ---------- helpers ---------- */

static xcb_atom_t intern(xcb_connection_t *c, const char *name)
{
    xcb_intern_atom_cookie_t ck = xcb_intern_atom(c, 0, strlen(name), name);
    xcb_intern_atom_reply_t *r  = xcb_intern_atom_reply(c, ck, NULL);
    if (!r) {
        return XCB_ATOM_NONE;
    }
    xcb_atom_t a = r->atom;
    free(r);
    return a;
}

/* ---------- D-Bus settings changed ---------- */

static void wm_on_settings_changed(const char *section, const char *key,
                                    void *user_data)
{
    (void)key;
    Wm *wm = (Wm *)user_data;
    if (strcmp(section, "appearance") == 0 ||
        strcmp(section, "wm.desktops") == 0 ||
        strcmp(section, "*") == 0) {
        /* Restart to pick up new config */
        wm->running = 0;
        wm->restart = 1;
    }
}

/* ---------- initialization ---------- */

int wm_init(Wm *wm, int *argc, char **argv)
{
    memset(wm, 0, sizeof(*wm));

    /* Initialize Xt — this opens the X connection for us */
    char **fallbacks = isde_theme_build_resources();
    wm->toplevel = IswAppInitialize(&wm->app, "ISDE-WM",
                                   NULL, 0,
                                   argc, argv,
                                   fallbacks,
                                   NULL, 0);

    wm->conn = IswDisplay(wm->toplevel);
    if (xcb_connection_has_error(wm->conn)) {
        fprintf(stderr, "isde-wm: cannot connect to X server\n");
        return -1;
    }

    /* Cache HiDPI scale factor and logical title bar height. */
    wm->scale_factor = ISWScaleFactor(wm->toplevel);
    if (wm->scale_factor < 1.0) { wm->scale_factor = 1.0; }
    wm->title_height = WM_TITLE_HEIGHT;

    wm->screen = IswScreen(wm->toplevel);
    wm->root = wm->screen->root;

    /* Determine screen number by iterating roots */
    wm->screen_num = 0;
    xcb_screen_iterator_t si = xcb_setup_roots_iterator(
        xcb_get_setup(wm->conn));
    for (int i = 0; si.rem; xcb_screen_next(&si), i++) {
        if (si.data->root == wm->root) {
            wm->screen_num = i;
            break;
        }
    }

    /* Claim window manager role via SubstructureRedirect on root */
    uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                    XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY   |
                    XCB_EVENT_MASK_STRUCTURE_NOTIFY      |
                    XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_void_cookie_t ck = xcb_change_window_attributes_checked(
        wm->conn, wm->root, XCB_CW_EVENT_MASK, &mask);
    xcb_generic_error_t *err = xcb_request_check(wm->conn, ck);
    if (err) {
        fprintf(stderr, "isde-wm: another window manager is running\n");
        free(err);
        return -1;
    }

    /* Intern atoms */
    wm->atom_wm_protocols      = intern(wm->conn, "WM_PROTOCOLS");
    wm->atom_wm_delete_window  = intern(wm->conn, "WM_DELETE_WINDOW");
    wm->atom_wm_take_focus     = intern(wm->conn, "WM_TAKE_FOCUS");
    wm->atom_wm_name           = intern(wm->conn, "WM_NAME");
    wm->atom_net_wm_name       = intern(wm->conn, "_NET_WM_NAME");
    wm->atom_motif_wm_hints    = intern(wm->conn, "_MOTIF_WM_HINTS");
    wm->atom_wm_change_state   = intern(wm->conn, "WM_CHANGE_STATE");

    /* Load initial colour scheme */
    isde_theme_current();

    /* Load cursor theme into RESOURCE_MANAGER so all X clients use it */
    {
        const char *theme = isde_cursor_theme_configured();
        const char *size  = isde_cursor_size_configured();
        if (theme || size) {
            char rdb[256];
            int rlen = 0;
            if (theme)
                rlen += snprintf(rdb + rlen, sizeof(rdb) - rlen,
                                 "Xcursor.theme:\t%s\n", theme);
            if (size)
                rlen += snprintf(rdb + rlen, sizeof(rdb) - rlen,
                                 "Xcursor.size:\t%s\n", size);
            xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE, wm->root,
                                intern(wm->conn, "RESOURCE_MANAGER"),
                                XCB_ATOM_STRING, 8, rlen, rdb);
            xcb_flush(wm->conn);
        }
    }

    /* EWMH, IPC, and D-Bus */
    wm->ewmh = isde_ewmh_init(wm->conn, wm->screen_num);
    wm->ipc  = isde_ipc_init(wm->conn, wm->screen_num);
    wm->dbus = isde_dbus_init();
    if (wm->dbus) {
        isde_dbus_settings_subscribe(wm->dbus, wm_on_settings_changed, wm);
    }

    /* Key bindings */
    wm->keysyms = xcb_key_symbols_alloc(wm->conn);
    wm_keys_setup(wm);

    /* EWMH setup (advertise _NET_SUPPORTED, etc.) */
    wm_ewmh_setup(wm);

    /* Virtual desktops */
    wm_desktops_init(wm);

    /* Manage any pre-existing windows */
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(
        wm->conn, xcb_query_tree(wm->conn, wm->root), NULL);
    if (tree) {
        xcb_window_t *children = xcb_query_tree_children(tree);
        int n = xcb_query_tree_children_length(tree);
        for (int i = 0; i < n; i++) {
            xcb_get_window_attributes_reply_t *attr =
                xcb_get_window_attributes_reply(
                    wm->conn,
                    xcb_get_window_attributes(wm->conn, children[i]),
                    NULL);
            if (attr) {
                if (!attr->override_redirect &&
                    attr->map_state == XCB_MAP_STATE_VIEWABLE) {
                    WmClient *c = frame_create(wm, children[i]);
                    if (c) {
                        c->desktop = wm->current_desktop;

                        /* Restore maximize state from previous WM session.
                         * The client's _NET_WM_STATE survives WM restarts;
                         * use it to re-maximize and set sane restore geometry. */
                        xcb_ewmh_get_atoms_reply_t state;
                        if (xcb_ewmh_get_wm_state_reply(
                                isde_ewmh_connection(wm->ewmh),
                                xcb_ewmh_get_wm_state(
                                    isde_ewmh_connection(wm->ewmh),
                                    children[i]),
                                &state, NULL)) {
                            int was_max = 0;
                            for (uint32_t s = 0; s < state.atoms_len; s++) {
                                if (state.atoms[s] == isde_ewmh_connection(wm->ewmh)->_NET_WM_STATE_MAXIMIZED_VERT ||
                                    state.atoms[s] == isde_ewmh_connection(wm->ewmh)->_NET_WM_STATE_MAXIMIZED_HORZ) {
                                    was_max = 1;
                                }
                            }
                            xcb_ewmh_get_atoms_reply_wipe(&state);
                            if (was_max) {
                                /* Current geometry is the maximized size;
                                 * use a centered default as restore target */
                                int wx, wy, ww, wh;
                                wm_get_work_area(wm, &wx, &wy, &ww, &wh);
                                c->save_x = wx + ww / 4;
                                c->save_y = wy + wh / 4;
                                c->save_w = ww / 2;
                                c->save_h = wh / 2;
                                c->maximized = 0;
                                wm_maximize_client(wm, c);
                            }
                        }

                        IswPopup(c->shell, IswGrabNone);
                        xcb_map_window(wm->conn, c->client);
                    }
                }
                free(attr);
            }
        }
        free(tree);
    }

    wm_ewmh_update_client_list(wm);
    xcb_flush(wm->conn);
    wm->running = 1;
    return 0;
}

/* ---------- client lookup ---------- */

WmClient *wm_find_client_by_frame(Wm *wm, xcb_window_t frame)
{
    for (WmClient *c = wm->clients; c; c = c->next) {
        if (c->shell && IswWindow(c->shell) == frame) {
            return c;
        }
    }
    return NULL;
}

WmClient *wm_find_client_by_widget(Wm *wm, Widget w)
{
    for (WmClient *c = wm->clients; c; c = c->next) {
        if (c->shell == w || c->title_label == w ||
            c->minimize_btn == w || c->maximize_btn == w ||
            c->close_btn == w) {
            return c;
        }
    }
    return NULL;
}

WmClient *wm_find_client_by_window(Wm *wm, xcb_window_t win)
{
    for (WmClient *c = wm->clients; c; c = c->next) {
        if (c->client == win) {
            return c;
        }
    }
    return NULL;
}

/* ---------- focus ---------- */

void wm_focus_client(Wm *wm, WmClient *c)
{
    WmClient *prev = wm->focused;
    wm->focused = c;

    if (prev && prev != c) {
        prev->focused = 0;
        frame_apply_theme(wm, prev);
        frame_update_title(wm, prev);
    }

    if (c) {
        c->focused = 1;
        c->focus_seq = ++wm->focus_seq;
        xcb_set_input_focus(wm->conn, XCB_INPUT_FOCUS_POINTER_ROOT,
                            c->client, XCB_CURRENT_TIME);
        /* Raise frame */
        fprintf(stderr, "isde-wm: focus+raise client 0x%x frame 0x%x\n",
                c->client, (unsigned)IswWindow(c->shell));
        uint32_t vals[] = { XCB_STACK_MODE_ABOVE };
        xcb_configure_window(wm->conn, IswWindow(c->shell),
                             XCB_CONFIG_WINDOW_STACK_MODE, vals);
        frame_apply_theme(wm, c);
        frame_update_title(wm, c);
    }
    wm_ewmh_update_active(wm);
    xcb_flush(wm->conn);
}

/* ---------- remove client ---------- */

static void snap_preview_hide(Wm *wm);

void wm_remove_client(Wm *wm, WmClient *c)
{
    WmClient **pp = &wm->clients;
    while (*pp && *pp != c) {
        pp = &(*pp)->next;
    }
    if (*pp) {
        *pp = c->next;
    }

    if (wm->focused == c) {
        wm->focused = NULL;
    }
    if (wm->drag_client == c) {
        snap_preview_hide(wm);
        wm->drag_client = NULL;
        wm->drag_mode = DRAG_NONE;
    }

    /* Cancel the window switcher if active — its client array is now stale */
    if (wm->switcher_active)
        wm_switcher_cancel(wm);

    frame_destroy(wm, c);
    wm_ewmh_update_client_list(wm);
    wm_ewmh_update_active(wm);

    if (!wm->focused && wm->clients) {
        /* Focus the most recently focused remaining client (MRU) */
        WmClient *mru = NULL;
        for (WmClient *p = wm->clients; p; p = p->next) {
            if (!mru || p->focus_seq > mru->focus_seq)
                mru = p;
        }
        wm_focus_client(wm, mru);
    }
}

/* ---------- close client via WM_DELETE_WINDOW ---------- */

static int client_supports_protocol(Wm *wm, WmClient *c, xcb_atom_t proto)
{
    xcb_icccm_get_wm_protocols_reply_t reply;
    if (!xcb_icccm_get_wm_protocols_reply(
            wm->conn,
            xcb_icccm_get_wm_protocols(wm->conn, c->client,
                                       wm->atom_wm_protocols),
            &reply, NULL))
        return 0;
    int found = 0;
    for (uint32_t i = 0; i < reply.atoms_len; i++) {
        if (reply.atoms[i] == proto) { found = 1; break; }
    }
    xcb_icccm_get_wm_protocols_reply_wipe(&reply);
    return found;
}

void wm_close_client(Wm *wm, WmClient *c)
{
    if (client_supports_protocol(wm, c, wm->atom_wm_delete_window)) {
        xcb_client_message_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.window = c->client;
        ev.type = wm->atom_wm_protocols;
        ev.format = 32;
        ev.data.data32[0] = wm->atom_wm_delete_window;
        ev.data.data32[1] = XCB_CURRENT_TIME;
        xcb_send_event(wm->conn, 0, c->client,
                       XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
    } else {
        xcb_kill_client(wm->conn, c->client);
    }
    xcb_flush(wm->conn);
}

/* ---------- work area (respects struts) ---------- */

static void check_strut(xcb_ewmh_connection_t *ewmh, xcb_window_t win,
                        int *top, int *bottom, int *left, int *right)
{
    xcb_ewmh_wm_strut_partial_t strut;
    if (xcb_ewmh_get_wm_strut_partial_reply(ewmh,
            xcb_ewmh_get_wm_strut_partial(ewmh, win),
            &strut, NULL)) {
        if ((int)strut.top > *top)       { *top = strut.top; }
        if ((int)strut.bottom > *bottom) { *bottom = strut.bottom; }
        if ((int)strut.left > *left)     { *left = strut.left; }
        if ((int)strut.right > *right)   { *right = strut.right; }
    }
}

void wm_get_work_area(Wm *wm, int *wx, int *wy, int *ww, int *wh)
{
    int top = 0, bottom = 0, left = 0, right = 0;
    xcb_ewmh_connection_t *ewmh = isde_ewmh_connection(wm->ewmh);

    /* Check managed clients — struts are on client windows, not frames */
    for (WmClient *c = wm->clients; c; c = c->next) {
        check_strut(ewmh, c->client, &top, &bottom, &left, &right);
    }

    /* Also check direct root children in case a window set struts
     * before being managed (e.g. override-redirect panels) */
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(
        wm->conn, xcb_query_tree(wm->conn, wm->root), NULL);
    if (tree) {
        xcb_window_t *children = xcb_query_tree_children(tree);
        int nchildren = xcb_query_tree_children_length(tree);
        for (int i = 0; i < nchildren; i++) {
            check_strut(ewmh, children[i], &top, &bottom, &left, &right);
        }
        free(tree);
    }

    /* Struts and screen dimensions are physical — convert to logical */
    double sf = wm->scale_factor;
    *wx = (int)(left / sf + 0.5);
    *wy = (int)(top / sf + 0.5);
    *ww = (int)((wm->screen->width_in_pixels - left - right) / sf + 0.5);
    *wh = (int)((wm->screen->height_in_pixels - top - bottom) / sf + 0.5);
}

/* ---------- snap detection ---------- */

enum {
    SNAP_NONE = 0,
    SNAP_LEFT, SNAP_RIGHT,
    SNAP_TL, SNAP_TR, SNAP_BL, SNAP_BR
};

#define SNAP_THRESHOLD 8

static int detect_snap_zone(Wm *wm, int rx, int ry)
{
    double sf = wm->scale_factor;
    int sw = phys_to_log(sf, wm->screen->width_in_pixels);
    int sh = phys_to_log(sf, wm->screen->height_in_pixels);
    int t = SNAP_THRESHOLD;

    int at_left   = (rx <= t);
    int at_right  = (rx >= sw - t - 1);
    int at_top    = (ry <= t);
    int at_bottom = (ry >= sh - t - 1);

    if (at_left  && at_top)    { return SNAP_TL; }
    if (at_right && at_top)    { return SNAP_TR; }
    if (at_left  && at_bottom) { return SNAP_BL; }
    if (at_right && at_bottom) { return SNAP_BR; }
    if (at_left)               { return SNAP_LEFT; }
    if (at_right)              { return SNAP_RIGHT; }
    return SNAP_NONE;
}

/* Compute the snap target geometry for a zone (does not modify client) */
static void snap_geometry(Wm *wm, int zone, int *sx, int *sy, int *sw, int *sh)
{
    int wx, wy, ww, wh;
    wm_get_work_area(wm, &wx, &wy, &ww, &wh);

    switch (zone) {
    case SNAP_LEFT:
        *sx = wx; *sy = wy;
        *sw = ww / 2; *sh = wh;
        break;
    case SNAP_RIGHT:
        *sx = wx + ww / 2; *sy = wy;
        *sw = ww - ww / 2; *sh = wh;
        break;
    case SNAP_TL:
        *sx = wx; *sy = wy;
        *sw = ww / 2; *sh = wh / 2;
        break;
    case SNAP_TR:
        *sx = wx + ww / 2; *sy = wy;
        *sw = ww - ww / 2; *sh = wh / 2;
        break;
    case SNAP_BL:
        *sx = wx; *sy = wy + wh / 2;
        *sw = ww / 2; *sh = wh / 2;
        break;
    case SNAP_BR:
        *sx = wx + ww / 2; *sy = wy + wh / 2;
        *sw = ww - ww / 2; *sh = wh / 2;
        break;
    default:
        *sx = *sy = *sw = *sh = 0;
        break;
    }
}

/* Show or reposition the snap preview overlay */
static void snap_preview_show(Wm *wm, int zone)
{
    int lx, ly, lw, lh;
    snap_geometry(wm, zone, &lx, &ly, &lw, &lh);
    if (lw <= 0 || lh <= 0) { return; }

    /* Inset by 2 logical px for a border-like appearance */
    int inset = 2;
    lx += inset; ly += inset;
    lw -= 2 * inset; lh -= 2 * inset;
    if (lw < 1) { lw = 1; }
    if (lh < 1) { lh = 1; }

    /* Convert to physical for the raw XCB overlay window */
    double sf = wm->scale_factor;
    int px = log_to_phys(sf, lx);
    int py = log_to_phys(sf, ly);
    int pw = log_to_phys(sf, lw);
    int ph = log_to_phys(sf, lh);

    /* Pick the active/accent colour from the theme */
    const IsdeColorScheme *s = isde_theme_current();
    unsigned int color = s ? s->active : 0x4488CC;

    if (!wm->snap_preview) {
        /* Allocate background pixel */
        xcb_alloc_color_reply_t *cr = xcb_alloc_color_reply(
            wm->conn,
            xcb_alloc_color(wm->conn, wm->screen->default_colormap,
                            ((color >> 16) & 0xFF) * 257,
                            ((color >> 8)  & 0xFF) * 257,
                            ( color        & 0xFF) * 257),
            NULL);
        uint32_t bg = cr ? cr->pixel : wm->screen->white_pixel;
        free(cr);

        wm->snap_preview = xcb_generate_id(wm->conn);
        uint32_t vals[] = {
            bg,
            1,  /* override-redirect */
            XCB_EVENT_MASK_NO_EVENT
        };
        xcb_create_window(wm->conn, XCB_COPY_FROM_PARENT,
                          wm->snap_preview, wm->root,
                          px, py, pw, ph, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          wm->screen->root_visual,
                          XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
                          XCB_CW_EVENT_MASK,
                          vals);
        /* Set 50% opacity via _NET_WM_WINDOW_OPACITY */
        uint32_t opacity = (uint32_t)(0.5 * 0xFFFFFFFF);
        xcb_atom_t atom_opacity = intern(wm->conn, "_NET_WM_WINDOW_OPACITY");
        xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE,
                            wm->snap_preview, atom_opacity,
                            XCB_ATOM_CARDINAL, 32, 1, &opacity);

        xcb_map_window(wm->conn, wm->snap_preview);
    } else {
        uint32_t cfg[] = { px, py, pw, ph };
        xcb_configure_window(wm->conn, wm->snap_preview,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                             cfg);
    }
    xcb_flush(wm->conn);
}

/* Hide and destroy the snap preview overlay */
static void snap_preview_hide(Wm *wm)
{
    if (wm->snap_preview) {
        xcb_destroy_window(wm->conn, wm->snap_preview);
        wm->snap_preview = 0;
        xcb_flush(wm->conn);
    }
    wm->snap_pending = SNAP_NONE;
}

static void apply_snap(Wm *wm, WmClient *c, int zone)
{
    int sx, sy, sw, sh;
    snap_geometry(wm, zone, &sx, &sy, &sw, &sh);

    int th = wm->title_height;

    /* Save geometry for restore */
    c->save_x = c->x;
    c->save_y = c->y;
    c->save_w = c->width;
    c->save_h = c->height;

    c->x = sx;
    c->y = sy;
    c->width = sw;
    c->height = sh - th;

    frame_configure(wm, c);
    xcb_flush(wm->conn);
}

/* ---------- maximize / minimize ---------- */

void wm_maximize_client(Wm *wm, WmClient *c)
{
    if (c->maximized) {
        c->x      = c->save_x;
        c->y      = c->save_y;
        c->width  = c->save_w;
        c->height = c->save_h;
        c->maximized = 0;
    } else {
        c->save_x = c->x;
        c->save_y = c->y;
        c->save_w = c->width;
        c->save_h = c->height;

        int wx, wy, ww, wh;
        wm_get_work_area(wm, &wx, &wy, &ww, &wh);
        c->x = wx;
        c->y = wy;
        int title = c->decorated ? wm->title_height : 0;
        c->width  = ww - 2 * WM_BORDER_WIDTH;
        c->height = wh - title - 2 * WM_BORDER_WIDTH;
        c->maximized = 1;
    }
    frame_configure(wm, c);

    /* Update _NET_WM_STATE so the client knows about the change */
    xcb_ewmh_connection_t *ewmh = isde_ewmh_connection(wm->ewmh);
    if (c->maximized) {
        xcb_atom_t states[] = {
            ewmh->_NET_WM_STATE_MAXIMIZED_VERT,
            ewmh->_NET_WM_STATE_MAXIMIZED_HORZ
        };
        xcb_ewmh_set_wm_state(ewmh, c->client, 2, states);
    } else {
        xcb_ewmh_set_wm_state(ewmh, c->client, 0, NULL);
    }

    xcb_flush(wm->conn);
}

void wm_minimize_client(Wm *wm, WmClient *c)
{
    /* Placeholder: unmap the frame. A proper implementation would
     * add the window to a taskbar/dock list for restoring later. */
    c->minimized = 1;
    if (c->shell) {
        IswPopdown(c->shell);
    }
    xcb_unmap_window(wm->conn, c->client);
    xcb_flush(wm->conn);

    if (wm->focused == c) {
        wm->focused = NULL;
        /* Focus next available client */
        for (WmClient *n = wm->clients; n; n = n->next) {
            if (n != c) {
                wm_focus_client(wm, n);
                break;
            }
        }
        wm_ewmh_update_active(wm);
    }
}

/* ---------- WM event handlers (non-widget events) ---------- */

/* Check _MOTIF_WM_HINTS to see if a client requests no decorations.
 * Returns 1 if the window should be decorated, 0 if not. */
#define MWM_HINTS_DECORATIONS (1 << 1)
int wm_client_wants_decorations(Wm *wm, xcb_window_t win)
{
    xcb_get_property_reply_t *reply = xcb_get_property_reply(
        wm->conn,
        xcb_get_property(wm->conn, 0, win,
                         wm->atom_motif_wm_hints,
                         XCB_ATOM_ANY,
                         0, 5),
        NULL);
    if (!reply) {
        return 1;
    }

    int dominated = 1;
    if (xcb_get_property_value_length(reply) >= 3 * (int)sizeof(uint32_t)) {
        uint32_t *hints = (uint32_t *)xcb_get_property_value(reply);
        uint32_t flags       = hints[0];
        uint32_t decorations = hints[2];
        if ((flags & MWM_HINTS_DECORATIONS) && decorations == 0) {
            dominated = 0;
        }
    }
    free(reply);
    return dominated;
}

/* Check _NET_WM_WINDOW_TYPE for types that should never be decorated */
int wm_window_type_wants_decorations(Wm *wm, xcb_window_t win)
{
    xcb_ewmh_connection_t *ewmh = isde_ewmh_connection(wm->ewmh);
    xcb_atom_t type = isde_ewmh_get_window_type(wm->ewmh, win);

    if (type == ewmh->_NET_WM_WINDOW_TYPE_DOCK    ||
        type == ewmh->_NET_WM_WINDOW_TYPE_DESKTOP  ||
        type == ewmh->_NET_WM_WINDOW_TYPE_SPLASH   ||
        type == ewmh->_NET_WM_WINDOW_TYPE_TOOLBAR   ||
        type == ewmh->_NET_WM_WINDOW_TYPE_MENU      ||
        type == ewmh->_NET_WM_WINDOW_TYPE_NOTIFICATION) {
        return 0;
    }
    return 1;
}

static void on_map_request(Wm *wm, xcb_map_request_event_t *ev)
{
    fprintf(stderr, "isde-wm: MapRequest for window 0x%x\n", ev->window);

    if (wm_find_client_by_window(wm, ev->window)) {
        return;
    }

    xcb_get_window_attributes_reply_t *attr =
        xcb_get_window_attributes_reply(
            wm->conn,
            xcb_get_window_attributes(wm->conn, ev->window),
            NULL);
    if (attr) {
        fprintf(stderr, "isde-wm:   override_redirect=%d map_state=%d\n",
                attr->override_redirect, attr->map_state);
        if (attr->override_redirect) {
            free(attr);
            return;
        }
        free(attr);
    }

    WmClient *c = frame_create(wm, ev->window);
    if (c) {
        c->desktop = wm->current_desktop;
        xcb_ewmh_set_wm_desktop(isde_ewmh_connection(wm->ewmh),
                                c->client, c->desktop);
        xcb_map_window(wm->conn, ev->window);
        IswPopup(c->shell, IswGrabNone);
        wm_focus_client(wm, c);
        wm_ewmh_update_client_list(wm);
        xcb_flush(wm->conn);
    }
}

static WmClient *find_grip_client(Wm *wm, xcb_window_t win, int *edge)
{
    for (WmClient *c = wm->clients; c; c = c->next) {
        for (int i = 0; i < 8; i++) {
            if (c->grip[i] == win) { *edge = i; return c; }
        }
    }
    *edge = -1;
    return NULL;
}

static void on_grip_press(Wm *wm, xcb_button_press_event_t *ev)
{
    int edge;
    WmClient *c = find_grip_client(wm, ev->event, &edge);
    if (!c || edge < 0) {
        return;
    }

    /* Raw XCB event — convert to logical */
    double sf = wm->scale_factor;
    wm_focus_client(wm, c);
    wm->drag_mode    = DRAG_RESIZE;
    wm->resize_edge  = edge;
    wm->drag_client  = c;
    wm->drag_start_x = phys_to_log(sf, ev->root_x);
    wm->drag_start_y = phys_to_log(sf, ev->root_y);
    wm->drag_orig_x  = c->x;
    wm->drag_orig_y  = c->y;
    wm->drag_orig_w  = c->width;
    wm->drag_orig_h  = c->height;

    xcb_grab_pointer(wm->conn, 1, wm->root,
                     XCB_EVENT_MASK_BUTTON_RELEASE |
                     XCB_EVENT_MASK_POINTER_MOTION,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                     XCB_NONE, wm->cursors[edge], XCB_CURRENT_TIME);
    xcb_flush(wm->conn);
}

static void on_configure_request(Wm *wm, xcb_configure_request_event_t *ev)
{
    WmClient *c = wm_find_client_by_window(wm, ev->window);
    if (c) {
        /* ConfigureRequest values are physical — convert to logical */
        double sf = wm->scale_factor;
        if (ev->value_mask & XCB_CONFIG_WINDOW_X) {
            c->x = phys_to_log(sf, ev->x);
        }
        if (ev->value_mask & XCB_CONFIG_WINDOW_Y) {
            c->y = phys_to_log(sf, ev->y);
        }
        if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
            c->width = phys_to_log(sf, ev->width);
        }
        if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
            c->height = phys_to_log(sf, ev->height);
        }
        frame_configure(wm, c);
    } else {
        uint32_t vals[7];
        int i = 0;
        uint16_t mask = ev->value_mask;
        if (mask & XCB_CONFIG_WINDOW_X)           { vals[i++] = ev->x; }
        if (mask & XCB_CONFIG_WINDOW_Y)           { vals[i++] = ev->y; }
        if (mask & XCB_CONFIG_WINDOW_WIDTH)       { vals[i++] = ev->width; }
        if (mask & XCB_CONFIG_WINDOW_HEIGHT)      { vals[i++] = ev->height; }
        if (mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) { vals[i++] = ev->border_width; }
        if (mask & XCB_CONFIG_WINDOW_SIBLING)     { vals[i++] = ev->sibling; }
        if (mask & XCB_CONFIG_WINDOW_STACK_MODE)  { vals[i++] = ev->stack_mode; }
        xcb_configure_window(wm->conn, ev->window, mask, vals);
    }
    xcb_flush(wm->conn);
}

static void on_unmap_notify(Wm *wm, xcb_unmap_notify_event_t *ev)
{
    WmClient *c = wm_find_client_by_window(wm, ev->window);
    if (c && c->shell && ev->event == IswWindow(c->shell)) {
        wm_remove_client(wm, c);
    }
}

static void on_destroy_notify(Wm *wm, xcb_destroy_notify_event_t *ev)
{
    WmClient *c = wm_find_client_by_window(wm, ev->window);
    if (c) {
        wm_remove_client(wm, c);
    }
}

static void on_button_release(Wm *wm, xcb_button_release_event_t *ev);

static void on_motion_notify(Wm *wm, xcb_motion_notify_event_t *ev)
{
    WmClient *c = wm->drag_client;
    if (!c || wm->drag_mode == DRAG_NONE) {
        return;
    }

    /* Raw XCB events are physical — convert to logical */
    double sf = wm->scale_factor;
    int rx = phys_to_log(sf, ev->root_x);
    int ry = phys_to_log(sf, ev->root_y);
    int dx = rx - wm->drag_start_x;
    int dy = ry - wm->drag_start_y;

    if (wm->drag_mode == DRAG_MOVE) {
        c->x = wm->drag_orig_x + dx;
        c->y = wm->drag_orig_y + dy;
        IswMoveWidget(c->shell, c->x, c->y);
        int zone = detect_snap_zone(wm, rx, ry);
        if (zone != SNAP_NONE) {
            if (wm->snap_pending != zone) {
                snap_preview_show(wm, zone);
                wm->snap_pending = zone;
            }
        } else if (wm->snap_pending != SNAP_NONE) {
            snap_preview_hide(wm);
        }
    } else if (wm->drag_mode == DRAG_RESIZE) {
        int e = wm->resize_edge;
        int nx = wm->drag_orig_x;
        int ny = wm->drag_orig_y;
        int nw = wm->drag_orig_w;
        int nh = wm->drag_orig_h;
        int min_sz = 50;

        if (e == GRIP_TOP || e == GRIP_TL || e == GRIP_TR) {
            nh = wm->drag_orig_h - dy;
            ny = wm->drag_orig_y + dy;
        }
        if (e == GRIP_BOTTOM || e == GRIP_BL || e == GRIP_BR) {
            nh = wm->drag_orig_h + dy;
        }
        if (e == GRIP_LEFT || e == GRIP_TL || e == GRIP_BL) {
            nw = wm->drag_orig_w - dx;
            nx = wm->drag_orig_x + dx;
        }
        if (e == GRIP_RIGHT || e == GRIP_TR || e == GRIP_BR) {
            nw = wm->drag_orig_w + dx;
        }

        if (nw < min_sz) { nw = min_sz; nx = wm->drag_orig_x + wm->drag_orig_w - min_sz; }
        if (nh < min_sz) { nh = min_sz; ny = wm->drag_orig_y + wm->drag_orig_h - min_sz; }

        c->x = nx; c->y = ny;
        c->width = nw; c->height = nh;
        frame_configure(wm, c);
    }

    xcb_flush(wm->conn);
}

static void on_button_release(Wm *wm, xcb_button_release_event_t *ev)
{
    (void)ev;
    if (wm->drag_mode != DRAG_NONE) {
        WmClient *c = wm->drag_client;

        if (wm->drag_mode == DRAG_MOVE && wm->snap_pending != SNAP_NONE && c) {
            apply_snap(wm, c, wm->snap_pending);
        }
        snap_preview_hide(wm);

        xcb_ungrab_pointer(wm->conn, XCB_CURRENT_TIME);
        xcb_flush(wm->conn);
        wm->drag_mode   = DRAG_NONE;
        wm->drag_client = NULL;
    }
}

static void on_property_notify(Wm *wm, xcb_property_notify_event_t *ev)
{
    WmClient *c = wm_find_client_by_window(wm, ev->window);
    if (!c) {
        return;
    }

    if (ev->atom == wm->atom_wm_name || ev->atom == wm->atom_net_wm_name) {
        frame_update_title(wm, c);
    } else if (ev->atom == wm->atom_motif_wm_hints) {
        int dominated = wm_client_wants_decorations(wm, c->client);
        if (dominated != c->decorated) {
            c->decorated = dominated;
            frame_configure(wm, c);
        }
    }
}

/* Returns 1 if handled, 0 if Xt should dispatch it */
static int on_client_message(Wm *wm, xcb_client_message_event_t *ev)
{
    xcb_ewmh_connection_t *ewmh = isde_ewmh_connection(wm->ewmh);

    if (ev->type == ewmh->_NET_ACTIVE_WINDOW) {
        WmClient *c = wm_find_client_by_window(wm, ev->window);
        if (!c) { return 1; }

        if (c->shell && !IswIsRealized(c->shell)) {
            IswRealizeWidget(c->shell);
        }
        IswPopup(c->shell, IswGrabNone);
        xcb_map_window(wm->conn, c->client);
        wm_focus_client(wm, c);
        return 1;
    } else if (ev->type == ewmh->_NET_CLOSE_WINDOW) {
        WmClient *c = wm_find_client_by_window(wm, ev->window);
        if (c) {
            wm_close_client(wm, c);
        }
        return 1;
    } else if (ev->type == ewmh->_NET_WM_STATE) {
        WmClient *c = wm_find_client_by_window(wm, ev->window);
        if (!c) { return 1; }

        uint32_t action = ev->data.data32[0];
        xcb_atom_t a1 = ev->data.data32[1];
        xcb_atom_t a2 = ev->data.data32[2];

        /* Check if either atom requests maximize */
        if (a1 == ewmh->_NET_WM_STATE_MAXIMIZED_VERT ||
            a1 == ewmh->_NET_WM_STATE_MAXIMIZED_HORZ ||
            a2 == ewmh->_NET_WM_STATE_MAXIMIZED_VERT ||
            a2 == ewmh->_NET_WM_STATE_MAXIMIZED_HORZ) {
            int want = (action == 1) || (action == 2 && !c->maximized);
            if (want != c->maximized) {
                wm_maximize_client(wm, c);
            }
        }
        /* Check if either atom requests minimize */
        if (a1 == ewmh->_NET_WM_STATE_HIDDEN ||
            a2 == ewmh->_NET_WM_STATE_HIDDEN) {
            int want = (action == 1) || (action == 2 && !c->minimized);
            if (want && !c->minimized) {
                wm_minimize_client(wm, c);
            }
        }
        return 1;
    } else if (ev->type == ewmh->_NET_WM_MOVERESIZE) {
        WmClient *c = wm_find_client_by_window(wm, ev->window);
        if (!c) { return 1; }

        /* EWMH moveresize coords are physical root pixels — convert */
        double sf = wm->scale_factor;
        int root_x   = phys_to_log(sf, (int)ev->data.data32[0]);
        int root_y   = phys_to_log(sf, (int)ev->data.data32[1]);
        uint32_t dir = ev->data.data32[2];

        if (dir == XCB_EWMH_WM_MOVERESIZE_CANCEL) {
            if (wm->drag_mode != DRAG_NONE) {
                snap_preview_hide(wm);
                xcb_ungrab_pointer(wm->conn, XCB_CURRENT_TIME);
                wm->drag_mode = DRAG_NONE;
                wm->drag_client = NULL;
                xcb_flush(wm->conn);
            }
            return 1;
        }

        wm->drag_client  = c;
        wm->drag_start_x = root_x;
        wm->drag_start_y = root_y;
        wm->drag_orig_x  = c->x;
        wm->drag_orig_y  = c->y;
        wm->drag_orig_w  = c->width;
        wm->drag_orig_h  = c->height;

        if (dir == XCB_EWMH_WM_MOVERESIZE_MOVE ||
            dir == XCB_EWMH_WM_MOVERESIZE_MOVE_KEYBOARD) {
            wm->drag_mode = DRAG_MOVE;
        } else if (dir <= XCB_EWMH_WM_MOVERESIZE_SIZE_LEFT) {
            /* Resize directions 0-7 map to edges */
            static const int dir_to_grip[] = {
                GRIP_TL, GRIP_TOP, GRIP_TR, GRIP_RIGHT,
                GRIP_BR, GRIP_BOTTOM, GRIP_BL, GRIP_LEFT
            };
            wm->drag_mode   = DRAG_RESIZE;
            wm->resize_edge = dir_to_grip[dir];
        } else {
            return 1;
        }

        xcb_grab_pointer(wm->conn, 1, wm->root,
                         XCB_EVENT_MASK_BUTTON_RELEASE |
                         XCB_EVENT_MASK_POINTER_MOTION,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                         XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
        xcb_flush(wm->conn);
        return 1;
    } else if (ev->type == wm->atom_wm_change_state) {
        /* ICCCM: WM_CHANGE_STATE with data[0] == IconicState → iconify */
        WmClient *c = wm_find_client_by_window(wm, ev->window);
        if (c && ev->data.data32[0] == 3) { /* IconicState = 3 */
            wm_minimize_client(wm, c);
        }
        return 1;
    }

    /* Not an EWMH message — let Xt handle (WM_PROTOCOLS, etc.) */
    return 0;
}

/* ---------- event loop ---------- */

static void dispatch_wm_event(Wm *wm, xcb_generic_event_t *ev)
{
    uint8_t type = ev->response_type & ~0x80;

    uint32_t cmd;
    if (isde_ipc_decode(wm->ipc, ev, &cmd, NULL, NULL, NULL, NULL)) {
        if (cmd == ISDE_CMD_QUIT) {
            wm->running = 0;
        }
        return;
    }

    switch (type) {
    case XCB_MAP_REQUEST:
        on_map_request(wm, (xcb_map_request_event_t *)ev);
        break;
    case XCB_CONFIGURE_REQUEST:
        on_configure_request(wm, (xcb_configure_request_event_t *)ev);
        break;
    case XCB_UNMAP_NOTIFY:
        on_unmap_notify(wm, (xcb_unmap_notify_event_t *)ev);
        break;
    case XCB_DESTROY_NOTIFY:
        on_destroy_notify(wm, (xcb_destroy_notify_event_t *)ev);
        break;
    case XCB_CLIENT_MESSAGE:
        if (!on_client_message(wm, (xcb_client_message_event_t *)ev)) {
            IswDispatchEvent(ev, wm->conn);
        }
        break;
    case XCB_MOTION_NOTIFY:
        if (wm->drag_mode != DRAG_NONE) {
            on_motion_notify(wm, (xcb_motion_notify_event_t *)ev);
        } else {
            IswDispatchEvent(ev, wm->conn);
        }
        break;
    case XCB_BUTTON_RELEASE:
        if (wm->drag_mode != DRAG_NONE) {
            on_button_release(wm, (xcb_button_release_event_t *)ev);
        } else {
            IswDispatchEvent(ev, wm->conn);
        }
        break;
    case XCB_BUTTON_PRESS: {
        xcb_button_press_event_t *bp = (xcb_button_press_event_t *)ev;
        int edge;
        if (find_grip_client(wm, bp->event, &edge)) {
            on_grip_press(wm, bp);
        } else {
            /* Click-to-focus: if the click is on a client window,
             * focus+raise it and replay the event to the client */
            WmClient *c = wm_find_client_by_window(wm, bp->event);
            if (c) {
                wm_focus_client(wm, c);
                xcb_allow_events(wm->conn, XCB_ALLOW_REPLAY_POINTER,
                                 bp->time);
                xcb_flush(wm->conn);
            } else {
                IswDispatchEvent(ev, wm->conn);
            }
        }
        break;
    }
    case XCB_PROPERTY_NOTIFY:
        on_property_notify(wm, (xcb_property_notify_event_t *)ev);
        break;
    case XCB_KEY_PRESS:
        wm_keys_handle(wm, (xcb_key_press_event_t *)ev);
        break;
    case XCB_KEY_RELEASE:
        wm_keys_handle_release(wm, (xcb_key_release_event_t *)ev);
        break;
    default:
        IswDispatchEvent(ev, wm->conn);
        break;
    }
}

void wm_run(Wm *wm)
{
    int xcb_fd = xcb_get_file_descriptor(wm->conn);

    while (wm->running && !IswAppGetExitFlag(wm->app)) {
        /* Process any pending Xt work (widget events, timers) */
        while (IswAppPending(wm->app)) {
            IswAppProcessEvent(wm->app, IswIMAll);
        }

        /* D-Bus dispatch */
        if (wm->dbus) {
            isde_dbus_dispatch(wm->dbus);
        }

        /* Wait for data on the XCB fd, with a short timeout
         * so Xt timers (OSD hide, etc.) get a chance to fire */
        struct pollfd pfd = { .fd = xcb_fd, .events = POLLIN };
        poll(&pfd, 1, 50);

        /* Drain all XCB events, coalescing consecutive motion events
         * so a drag resize/move only processes the latest position. */
        xcb_generic_event_t *ev;
        xcb_generic_event_t *held_motion = NULL;
        while ((ev = xcb_poll_for_event(wm->conn))) {
            uint8_t type = ev->response_type & ~0x80;
            if (type == XCB_MOTION_NOTIFY && wm->drag_mode != DRAG_NONE) {
                free(held_motion);
                held_motion = ev;
                continue;
            }
            if (held_motion) {
                dispatch_wm_event(wm, held_motion);
                free(held_motion);
                held_motion = NULL;
            }
            dispatch_wm_event(wm, ev);
            free(ev);
        }
        if (held_motion) {
            dispatch_wm_event(wm, held_motion);
            free(held_motion);
        }
    }
}

/* ---------- cleanup ---------- */

void wm_cleanup(Wm *wm)
{
    while (wm->clients) {
        wm_remove_client(wm, wm->clients);
    }
    xcb_flush(wm->conn);

    if (wm->keysyms) {
        xcb_key_symbols_free(wm->keysyms);
    }
    isde_dbus_free(wm->dbus);
    isde_ipc_free(wm->ipc);
    isde_ewmh_free(wm->ewmh);

    IswDestroyApplicationContext(wm->app);
}