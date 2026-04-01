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
        ewmh->_NET_ACTIVE_WINDOW,
        ewmh->_NET_WM_NAME,
        ewmh->_NET_WM_STATE,
        ewmh->_NET_WM_WINDOW_TYPE,
        ewmh->_NET_CLOSE_WINDOW,
        ewmh->_NET_NUMBER_OF_DESKTOPS,
        ewmh->_NET_CURRENT_DESKTOP,
        ewmh->_NET_WM_STRUT_PARTIAL,
        ewmh->_NET_WM_DESKTOP,
    };
    int nsupported = sizeof(supported) / sizeof(supported[0]);
    isde_ewmh_set_supported(wm->ewmh, supported, nsupported);

    /* Desktop count set by wm_desktops_init() */
    isde_ewmh_set_client_list(wm->ewmh, NULL, 0);
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

void wm_ewmh_update_active(Wm *wm)
{
    xcb_window_t active = wm->focused ? wm->focused->client
                                      : XCB_WINDOW_NONE;
    isde_ewmh_set_active_window(wm->ewmh, active);
}
