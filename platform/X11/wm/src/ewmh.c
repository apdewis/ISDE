/*
 * ewmh.c — EWMH property management for the window manager
 */
#include "wm.h"

#include <stdlib.h>
#define _POSIX_C_SOURCE 200809L
/*
 * isde-ewmh.c — EWMH / ICCCM atom cache and helpers
 */
#include "isde/isde-ewmh.h"

#include <stdlib.h>
#include <string.h>
#include <xcb/xcb_aux.h>

struct IsdeEwmh {
    xcb_connection_t      *conn;
    xcb_ewmh_connection_t  ewmh;
    xcb_screen_t          *screen;
    int                    screen_num;
    xcb_atom_t             startup_info_begin;
    xcb_atom_t             startup_info;
    xcb_atom_t             startup_id;
};

IsdeEwmh *isde_ewmh_init(xcb_connection_t *conn, int screen)
{
    IsdeEwmh *e = calloc(1, sizeof(*e));
    if (!e) {
        return NULL;
    }

    e->conn = conn;
    e->screen_num = screen;

    xcb_intern_atom_cookie_t *cookies = xcb_ewmh_init_atoms(conn, &e->ewmh);
    if (!cookies) {
        free(e);
        return NULL;
    }
    if (!xcb_ewmh_init_atoms_replies(&e->ewmh, cookies, NULL)) {
        free(e);
        return NULL;
    }

    e->screen = xcb_aux_get_screen(conn, screen);

    xcb_intern_atom_cookie_t sib_ck = xcb_intern_atom(conn, 0, 24,
        "_NET_STARTUP_INFO_BEGIN");
    xcb_intern_atom_cookie_t si_ck = xcb_intern_atom(conn, 0, 18,
        "_NET_STARTUP_INFO");
    xcb_intern_atom_cookie_t sid_ck = xcb_intern_atom(conn, 0, 16,
        "_NET_STARTUP_ID");

    xcb_intern_atom_reply_t *sib_r = xcb_intern_atom_reply(conn, sib_ck, NULL);
    xcb_intern_atom_reply_t *si_r = xcb_intern_atom_reply(conn, si_ck, NULL);
    xcb_intern_atom_reply_t *sid_r = xcb_intern_atom_reply(conn, sid_ck, NULL);

    e->startup_info_begin = sib_r ? sib_r->atom : XCB_ATOM_NONE;
    e->startup_info       = si_r  ? si_r->atom  : XCB_ATOM_NONE;
    e->startup_id         = sid_r ? sid_r->atom  : XCB_ATOM_NONE;

    free(sib_r);
    free(si_r);
    free(sid_r);

    return e;
}

void isde_ewmh_free(IsdeEwmh *e)
{
    if (!e) {
        return;
    }
    xcb_ewmh_connection_wipe(&e->ewmh);
    free(e);
}

xcb_ewmh_connection_t *isde_ewmh_connection(IsdeEwmh *e) { return &e->ewmh; }
xcb_connection_t      *isde_ewmh_xcb(IsdeEwmh *e)        { return e->conn; }
xcb_screen_t          *isde_ewmh_screen(IsdeEwmh *e)     { return e->screen; }
xcb_window_t           isde_ewmh_root(IsdeEwmh *e)       { return e->screen->root; }

int isde_ewmh_set_supported(IsdeEwmh *e, xcb_atom_t *atoms, int count)
{
    xcb_ewmh_set_supported(&e->ewmh, e->screen_num, count, atoms);
    xcb_flush(e->conn);
    return 1;
}

int isde_ewmh_set_client_list(IsdeEwmh *e, xcb_window_t *wins, int count)
{
    xcb_ewmh_set_client_list(&e->ewmh, e->screen_num, count, wins);
    xcb_flush(e->conn);
    return 1;
}

int isde_ewmh_set_client_list_stacking(IsdeEwmh *e, xcb_window_t *wins, int count)
{
    xcb_ewmh_set_client_list_stacking(&e->ewmh, e->screen_num, count, wins);
    xcb_flush(e->conn);
    return 1;
}

int isde_ewmh_set_active_window(IsdeEwmh *e, xcb_window_t win)
{
    xcb_ewmh_set_active_window(&e->ewmh, e->screen_num, win);
    xcb_flush(e->conn);
    return 1;
}

int isde_ewmh_set_number_of_desktops(IsdeEwmh *e, uint32_t n)
{
    xcb_ewmh_set_number_of_desktops(&e->ewmh, e->screen_num, n);
    xcb_flush(e->conn);
    return 1;
}

int isde_ewmh_set_current_desktop(IsdeEwmh *e, uint32_t desk)
{
    xcb_ewmh_set_current_desktop(&e->ewmh, e->screen_num, desk);
    xcb_flush(e->conn);
    return 1;
}

int isde_ewmh_set_wm_name(IsdeEwmh *e, xcb_window_t win, const char *name)
{
    size_t len = strlen(name);
    xcb_ewmh_set_wm_name(&e->ewmh, win, len, name);
    xcb_flush(e->conn);
    return 1;
}

xcb_window_t isde_ewmh_get_active_window(IsdeEwmh *e)
{
    xcb_window_t win = XCB_WINDOW_NONE;
    xcb_ewmh_get_active_window_reply(
        &e->ewmh,
        xcb_ewmh_get_active_window(&e->ewmh, e->screen_num),
        &win, NULL);
    return win;
}

uint32_t isde_ewmh_get_current_desktop(IsdeEwmh *e)
{
    uint32_t desk = 0;
    xcb_ewmh_get_current_desktop_reply(
        &e->ewmh,
        xcb_ewmh_get_current_desktop(&e->ewmh, e->screen_num),
        &desk, NULL);
    return desk;
}

uint32_t isde_ewmh_get_number_of_desktops(IsdeEwmh *e)
{
    uint32_t n = 1;
    xcb_ewmh_get_number_of_desktops_reply(
        &e->ewmh,
        xcb_ewmh_get_number_of_desktops(&e->ewmh, e->screen_num),
        &n, NULL);
    return n;
}

int isde_ewmh_get_client_list(IsdeEwmh *e, xcb_window_t **wins)
{
    xcb_ewmh_get_windows_reply_t reply;
    if (!xcb_ewmh_get_client_list_reply(
            &e->ewmh,
            xcb_ewmh_get_client_list(&e->ewmh, e->screen_num),
            &reply, NULL)) {
        return 0;
    }

    int count = reply.windows_len;
    *wins = malloc(count * sizeof(xcb_window_t));
    if (*wins) {
        memcpy(*wins, reply.windows, count * sizeof(xcb_window_t));
    } else {
        count = 0;
    }
    xcb_ewmh_get_windows_reply_wipe(&reply);
    return count;
}

int isde_ewmh_get_client_list_stacking(IsdeEwmh *e, xcb_window_t **wins)
{
    xcb_ewmh_get_windows_reply_t reply;
    if (!xcb_ewmh_get_client_list_stacking_reply(
            &e->ewmh,
            xcb_ewmh_get_client_list_stacking(&e->ewmh, e->screen_num),
            &reply, NULL)) {
        return 0;
    }

    int count = reply.windows_len;
    *wins = malloc(count * sizeof(xcb_window_t));
    if (*wins) {
        memcpy(*wins, reply.windows, count * sizeof(xcb_window_t));
    } else {
        count = 0;
    }
    xcb_ewmh_get_windows_reply_wipe(&reply);
    return count;
}

xcb_atom_t isde_ewmh_get_window_type(IsdeEwmh *e, xcb_window_t win)
{
    xcb_ewmh_get_atoms_reply_t reply;
    if (!xcb_ewmh_get_wm_window_type_reply(
            &e->ewmh,
            xcb_ewmh_get_wm_window_type(&e->ewmh, win),
            &reply, NULL)) {
        return XCB_ATOM_NONE;
    }

    xcb_atom_t type = reply.atoms_len > 0 ? reply.atoms[0] : XCB_ATOM_NONE;
    xcb_ewmh_get_atoms_reply_wipe(&reply);
    return type;
}

int isde_ewmh_get_wm_class(IsdeEwmh *e, xcb_window_t win,
                            char **instance_out, char **class_out)
{
    xcb_icccm_get_wm_class_reply_t reply;
    if (!xcb_icccm_get_wm_class_reply(
            e->conn,
            xcb_icccm_get_wm_class(e->conn, win),
            &reply, NULL)) {
        return 0;
    }

    *instance_out = strdup(reply.instance_name);
    *class_out = strdup(reply.class_name);
    xcb_icccm_get_wm_class_reply_wipe(&reply);
    return 1;
}

uint32_t isde_ewmh_get_wm_desktop(IsdeEwmh *e, xcb_window_t win)
{
    uint32_t desk = 0xFFFFFFFF;
    xcb_ewmh_get_wm_desktop_reply(
        &e->ewmh,
        xcb_ewmh_get_wm_desktop(&e->ewmh, win),
        &desk, NULL);
    return desk;
}

void isde_ewmh_request_active_window(IsdeEwmh *e, xcb_window_t win)
{
    xcb_ewmh_request_change_active_window(
        &e->ewmh, e->screen_num, win,
        XCB_EWMH_CLIENT_SOURCE_TYPE_OTHER,
        XCB_CURRENT_TIME, XCB_WINDOW_NONE);
    xcb_flush(e->conn);
}

void isde_ewmh_request_close_window(IsdeEwmh *e, xcb_window_t win)
{
    xcb_ewmh_request_close_window(
        &e->ewmh, e->screen_num, win,
        XCB_CURRENT_TIME,
        XCB_EWMH_CLIENT_SOURCE_TYPE_OTHER);
    xcb_flush(e->conn);
}

int isde_ewmh_get_workarea(IsdeEwmh *e, int *x, int *y, int *w, int *h)
{
    xcb_ewmh_get_workarea_reply_t reply;
    if (xcb_ewmh_get_workarea_reply(
            &e->ewmh,
            xcb_ewmh_get_workarea(&e->ewmh, e->screen_num),
            &reply, NULL)) {
        uint32_t desk = 0;
        xcb_ewmh_get_current_desktop_reply(
            &e->ewmh,
            xcb_ewmh_get_current_desktop(&e->ewmh, e->screen_num),
            &desk, NULL);
        if (desk < reply.workarea_len) {
            *x = reply.workarea[desk].x;
            *y = reply.workarea[desk].y;
            *w = reply.workarea[desk].width;
            *h = reply.workarea[desk].height;
            xcb_ewmh_get_workarea_reply_wipe(&reply);
            return 1;
        }
        xcb_ewmh_get_workarea_reply_wipe(&reply);
    }

    /* Fallback: use screen geometry */
    *x = 0;
    *y = 0;
    *w = e->screen->width_in_pixels;
    *h = e->screen->height_in_pixels;
    return 1;
}

void isde_ewmh_request_current_desktop(IsdeEwmh *e, uint32_t desktop)
{
    xcb_ewmh_request_change_current_desktop(&e->ewmh, e->screen_num,
                                            desktop, XCB_CURRENT_TIME);
    xcb_flush(e->conn);
}

void isde_ewmh_request_wm_desktop(IsdeEwmh *e, xcb_window_t win,
                                  uint32_t desktop)
{
    xcb_ewmh_request_change_wm_desktop(&e->ewmh, e->screen_num, win,
                                       desktop,
                                       XCB_EWMH_CLIENT_SOURCE_TYPE_OTHER);
    xcb_flush(e->conn);
}

void isde_ewmh_set_desktop_layout(IsdeEwmh *e, int orientation,
                                  int cols, int rows, int starting_corner)
{
    xcb_ewmh_set_desktop_layout(&e->ewmh, e->screen_num,
                                orientation, cols, rows, starting_corner);
    xcb_flush(e->conn);
}

int isde_ewmh_get_desktop_layout(IsdeEwmh *e, int *orientation,
                                 int *cols, int *rows, int *starting_corner)
{
    xcb_ewmh_get_desktop_layout_reply_t layout;
    if (!xcb_ewmh_get_desktop_layout_reply(
            &e->ewmh,
            xcb_ewmh_get_desktop_layout(&e->ewmh, e->screen_num),
            &layout, NULL)) {
        return 0;
    }
    *orientation = layout.orientation;
    *cols = layout.columns;
    *rows = layout.rows;
    *starting_corner = layout.starting_corner;
    return 1;
}

xcb_atom_t isde_ewmh_atom_startup_info_begin(IsdeEwmh *e) { return e->startup_info_begin; }
xcb_atom_t isde_ewmh_atom_startup_info(IsdeEwmh *e)       { return e->startup_info; }
xcb_atom_t isde_ewmh_atom_startup_id(IsdeEwmh *e)         { return e->startup_id; }

void isde_clamp_to_workarea(xcb_connection_t *conn, int screen,
                            int *w, int *h)
{
    IsdeEwmh *e = isde_ewmh_init(conn, screen);
    if (!e) return;

    int wx, wy, ww, wh;
    if (isde_ewmh_get_workarea(e, &wx, &wy, &ww, &wh)) {
        if (*w > ww) *w = ww;
        if (*h > wh) *h = wh;
    }
    isde_ewmh_free(e);
}

void wm_ewmh_setup(Wm *wm)
{
    xcb_ewmh_connection_t *ewmh = isde_ewmh_connection(wm->ewmh);

    /* Create a child window for _NET_SUPPORTING_WM_CHECK */
    xcb_window_t check_win = xcb_generate_id(wm->conn);
    xcb_create_window(wm->conn, XCB_COPY_FROM_PARENT, check_win, wm->root,
                      -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      wm->screen->root_visual, 0, NULL);

    xcb_ewmh_set_supporting_wm_check(ewmh, wm->screen_num, check_win);
    xcb_ewmh_set_supporting_wm_check(ewmh, check_win, check_win);
    xcb_ewmh_set_wm_name(ewmh, check_win, 7, "isde-wm");

    xcb_atom_t supported[] = {
        ewmh->_NET_SUPPORTED,
        ewmh->_NET_SUPPORTING_WM_CHECK,
        ewmh->_NET_CLIENT_LIST,
        ewmh->_NET_CLIENT_LIST_STACKING,
        ewmh->_NET_ACTIVE_WINDOW,
        ewmh->_NET_WM_NAME,
        ewmh->_NET_WM_STATE,
        ewmh->_NET_WM_WINDOW_TYPE,
        ewmh->_NET_CLOSE_WINDOW,
        ewmh->_NET_NUMBER_OF_DESKTOPS,
        ewmh->_NET_CURRENT_DESKTOP,
        ewmh->_NET_WM_STRUT_PARTIAL,
        ewmh->_NET_WM_DESKTOP,
        ewmh->_NET_WM_STATE_FULLSCREEN,
        ewmh->_NET_WM_STATE_ABOVE,
        ewmh->_NET_WM_STATE_BELOW,
        ewmh->_NET_WM_STATE_HIDDEN,
        ewmh->_NET_WM_STATE_MAXIMIZED_VERT,
        ewmh->_NET_WM_STATE_MAXIMIZED_HORZ,
        ewmh->_NET_WM_STATE_MODAL,
        ewmh->_NET_WM_STATE_STICKY,
        ewmh->_NET_WM_STATE_SKIP_TASKBAR,
        ewmh->_NET_WM_STATE_SKIP_PAGER,
        ewmh->_NET_WM_STATE_DEMANDS_ATTENTION,
        wm->atom_net_wm_state_focused,
        ewmh->_NET_WORKAREA,
        ewmh->_NET_FRAME_EXTENTS,
        ewmh->_NET_WM_MOVERESIZE,
        ewmh->_NET_DESKTOP_LAYOUT,
        ewmh->_NET_WM_ALLOWED_ACTIONS,
        ewmh->_NET_WM_VISIBLE_NAME,
        ewmh->_NET_WM_VISIBLE_ICON_NAME,
        wm->atom_net_wm_user_time,
        wm->atom_net_wm_user_time_window,
        wm->atom_net_startup_info_begin,
        wm->atom_net_startup_info,
        wm->atom_net_startup_id,
    };
    int nsupported = sizeof(supported) / sizeof(supported[0]);
    isde_ewmh_set_supported(wm->ewmh, supported, nsupported);

    /* Desktop count set by wm_desktops_init() */
    isde_ewmh_set_client_list(wm->ewmh, NULL, 0);
    isde_ewmh_set_client_list_stacking(wm->ewmh, NULL, 0);
    isde_ewmh_set_active_window(wm->ewmh, XCB_WINDOW_NONE);

    xcb_flush(wm->conn);
}

void wm_ewmh_update_client_list(Wm *wm)
{
    int count = 0;
    for (WmClient *c = wm->clients; c; c = c->next) {
        if (!c->transient_for)
            count++;
    }

    xcb_window_t *wins = NULL;
    if (count > 0) {
        wins = malloc(count * sizeof(xcb_window_t));
        if (!wins) {
            return;
        }
        int i = 0;
        for (WmClient *c = wm->clients; c; c = c->next) {
            if (!c->transient_for)
                wins[i++] = c->client;
        }
    }

    isde_ewmh_set_client_list(wm->ewmh, wins, count);
    free(wins);
}

static int cmp_stacking(const void *a, const void *b)
{
    const WmClient *ca = *(const WmClient *const *)a;
    const WmClient *cb = *(const WmClient *const *)b;
    if (ca->below != cb->below) { return ca->below ? -1 : 1; }
    if (ca->above != cb->above) { return ca->above ? 1 : -1; }
    if (ca->focus_seq < cb->focus_seq) { return -1; }
    if (ca->focus_seq > cb->focus_seq) { return 1; }
    return 0;
}

void wm_ewmh_update_client_list_stacking(Wm *wm)
{
    int count = 0;
    for (WmClient *c = wm->clients; c; c = c->next) {
        if (!c->transient_for) {
            count++;
        }
    }

    if (count == 0) {
        isde_ewmh_set_client_list_stacking(wm->ewmh, NULL, 0);
        return;
    }

    WmClient **sorted = malloc(count * sizeof(WmClient *));
    xcb_window_t *wins = malloc(count * sizeof(xcb_window_t));
    if (!sorted || !wins) {
        free(sorted);
        free(wins);
        return;
    }

    int idx = 0;
    for (WmClient *c = wm->clients; c; c = c->next) {
        if (!c->transient_for) {
            sorted[idx++] = c;
        }
    }

    qsort(sorted, count, sizeof(WmClient *), cmp_stacking);

    for (int i = 0; i < count; i++) {
        wins[i] = sorted[i]->client;
    }

    free(sorted);
    isde_ewmh_set_client_list_stacking(wm->ewmh, wins, count);
    free(wins);
}

void wm_ewmh_update_active(Wm *wm)
{
    xcb_window_t active = wm->focused ? wm->focused->client
                                      : XCB_WINDOW_NONE;
    isde_ewmh_set_active_window(wm->ewmh, active);
}

void wm_ewmh_update_workarea(Wm *wm)
{
    /* _NET_WORKAREA must be in physical (root window) pixels per EWMH.
     * Compute struts directly without the logical conversion. */
    int top = 0, bottom = 0, left = 0, right = 0;
    xcb_ewmh_connection_t *ewmh = isde_ewmh_connection(wm->ewmh);

    for (WmClient *c = wm->clients; c; c = c->next) {
        xcb_ewmh_wm_strut_partial_t strut;
        if (xcb_ewmh_get_wm_strut_partial_reply(ewmh,
                xcb_ewmh_get_wm_strut_partial(ewmh, c->client),
                &strut, NULL)) {
            if ((int)strut.top > top)       { top = strut.top; }
            if ((int)strut.bottom > bottom) { bottom = strut.bottom; }
            if ((int)strut.left > left)     { left = strut.left; }
            if ((int)strut.right > right)   { right = strut.right; }
        }
    }

    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(
        wm->conn, xcb_query_tree(wm->conn, wm->root), NULL);
    if (tree) {
        xcb_window_t *children = xcb_query_tree_children(tree);
        int nchildren = xcb_query_tree_children_length(tree);
        for (int i = 0; i < nchildren; i++) {
            xcb_ewmh_wm_strut_partial_t strut;
            if (xcb_ewmh_get_wm_strut_partial_reply(ewmh,
                    xcb_ewmh_get_wm_strut_partial(ewmh, children[i]),
                    &strut, NULL)) {
                if ((int)strut.top > top)       { top = strut.top; }
                if ((int)strut.bottom > bottom) { bottom = strut.bottom; }
                if ((int)strut.left > left)     { left = strut.left; }
                if ((int)strut.right > right)   { right = strut.right; }
            }
        }
        free(tree);
    }

    int wx = left;
    int wy = top;
    int ww = wm->screen->width_in_pixels - left - right;
    int wh = wm->screen->height_in_pixels - top - bottom;

    xcb_ewmh_geometry_t *areas = malloc(
        wm->num_desktops * sizeof(xcb_ewmh_geometry_t));
    if (!areas)
        return;

    for (int i = 0; i < wm->num_desktops; i++) {
        areas[i].x      = wx;
        areas[i].y      = wy;
        areas[i].width   = ww;
        areas[i].height  = wh;
    }

    xcb_ewmh_set_workarea(ewmh, wm->screen_num,
                          wm->num_desktops, areas);
    free(areas);
    xcb_flush(wm->conn);
}

void wm_ewmh_set_allowed_actions(Wm *wm, WmClient *c)
{
    xcb_ewmh_connection_t *ewmh = isde_ewmh_connection(wm->ewmh);
    xcb_atom_t actions[10];
    int n = 0;

    if (c->decorated) {
        actions[n++] = ewmh->_NET_WM_ACTION_MOVE;
        if (!c->fixed_size) {
            actions[n++] = ewmh->_NET_WM_ACTION_RESIZE;
            actions[n++] = ewmh->_NET_WM_ACTION_MAXIMIZE_HORZ;
            actions[n++] = ewmh->_NET_WM_ACTION_MAXIMIZE_VERT;
        }
    }
    actions[n++] = ewmh->_NET_WM_ACTION_MINIMIZE;
    actions[n++] = ewmh->_NET_WM_ACTION_FULLSCREEN;
    actions[n++] = ewmh->_NET_WM_ACTION_CHANGE_DESKTOP;
    actions[n++] = ewmh->_NET_WM_ACTION_CLOSE;
    actions[n++] = ewmh->_NET_WM_ACTION_ABOVE;
    actions[n++] = ewmh->_NET_WM_ACTION_BELOW;

    xcb_ewmh_set_wm_allowed_actions(ewmh, c->client, n, actions);
}
