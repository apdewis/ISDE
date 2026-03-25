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
};

IsdeEwmh *isde_ewmh_init(xcb_connection_t *conn, int screen)
{
    IsdeEwmh *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;

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
    return e;
}

void isde_ewmh_free(IsdeEwmh *e)
{
    if (!e)
        return;
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
            &reply, NULL))
        return 0;

    int count = reply.windows_len;
    *wins = malloc(count * sizeof(xcb_window_t));
    if (*wins)
        memcpy(*wins, reply.windows, count * sizeof(xcb_window_t));
    else
        count = 0;
    xcb_ewmh_get_windows_reply_wipe(&reply);
    return count;
}

xcb_atom_t isde_ewmh_get_window_type(IsdeEwmh *e, xcb_window_t win)
{
    xcb_ewmh_get_atoms_reply_t reply;
    if (!xcb_ewmh_get_wm_window_type_reply(
            &e->ewmh,
            xcb_ewmh_get_wm_window_type(&e->ewmh, win),
            &reply, NULL))
        return XCB_ATOM_NONE;

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
            &reply, NULL))
        return 0;

    *instance_out = strdup(reply.instance_name);
    *class_out = strdup(reply.class_name);
    xcb_icccm_get_wm_class_reply_wipe(&reply);
    return 1;
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
