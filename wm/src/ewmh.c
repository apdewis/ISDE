/*
 * ewmh.c — EWMH property management for the window manager
 */
#include "wm.h"

#include <stdlib.h>

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
        ewmh->_NET_WORKAREA,
        ewmh->_NET_FRAME_EXTENTS,
        ewmh->_NET_WM_MOVERESIZE,
        ewmh->_NET_DESKTOP_LAYOUT,
        ewmh->_NET_WM_ALLOWED_ACTIONS,
        ewmh->_NET_WM_VISIBLE_NAME,
        ewmh->_NET_WM_VISIBLE_ICON_NAME,
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
