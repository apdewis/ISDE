#define _POSIX_C_SOURCE 200809L
/*
 * isde-ewmh-x11.c — X11 backend: EWMH / ICCCM ops.
 *
 * Absorbs the body of the former isde-ewmh.c, reshaped to operate on the
 * backend's IsdeDisplay (which owns the connection + EWMH atom cache).
 */
#include "isde-platform-x11.h"

#include <stdlib.h>
#include <string.h>
#include <xcb/xcb_icccm.h>

static int set_supported(IsdeDisplay *d, const IsdeAtom *atoms, int count)
{
    xcb_ewmh_set_supported(&d->ewmh, d->screen_num, count, (xcb_atom_t *)atoms);
    xcb_flush(d->conn);
    return 1;
}

static int set_client_list(IsdeDisplay *d, const IsdeWindow *wins, int count)
{
    xcb_ewmh_set_client_list(&d->ewmh, d->screen_num, count, (xcb_window_t *)wins);
    xcb_flush(d->conn);
    return 1;
}

static int set_client_list_stacking(IsdeDisplay *d, const IsdeWindow *wins, int count)
{
    xcb_ewmh_set_client_list_stacking(&d->ewmh, d->screen_num, count,
                                      (xcb_window_t *)wins);
    xcb_flush(d->conn);
    return 1;
}

static int set_active_window(IsdeDisplay *d, IsdeWindow win)
{
    xcb_ewmh_set_active_window(&d->ewmh, d->screen_num, win);
    xcb_flush(d->conn);
    return 1;
}

static int set_number_of_desktops(IsdeDisplay *d, uint32_t n)
{
    xcb_ewmh_set_number_of_desktops(&d->ewmh, d->screen_num, n);
    xcb_flush(d->conn);
    return 1;
}

static int set_current_desktop(IsdeDisplay *d, uint32_t desk)
{
    xcb_ewmh_set_current_desktop(&d->ewmh, d->screen_num, desk);
    xcb_flush(d->conn);
    return 1;
}

static int set_wm_name(IsdeDisplay *d, IsdeWindow win, const char *name)
{
    xcb_ewmh_set_wm_name(&d->ewmh, win, strlen(name), name);
    xcb_flush(d->conn);
    return 1;
}

static IsdeWindow get_active_window(IsdeDisplay *d)
{
    xcb_window_t win = XCB_WINDOW_NONE;
    xcb_ewmh_get_active_window_reply(
        &d->ewmh,
        xcb_ewmh_get_active_window(&d->ewmh, d->screen_num),
        &win, NULL);
    return win;
}

static uint32_t get_current_desktop(IsdeDisplay *d)
{
    uint32_t desk = 0;
    xcb_ewmh_get_current_desktop_reply(
        &d->ewmh,
        xcb_ewmh_get_current_desktop(&d->ewmh, d->screen_num),
        &desk, NULL);
    return desk;
}

static uint32_t get_number_of_desktops(IsdeDisplay *d)
{
    uint32_t n = 1;
    xcb_ewmh_get_number_of_desktops_reply(
        &d->ewmh,
        xcb_ewmh_get_number_of_desktops(&d->ewmh, d->screen_num),
        &n, NULL);
    return n;
}

static int get_client_list(IsdeDisplay *d, IsdeWindow **wins)
{
    xcb_ewmh_get_windows_reply_t reply;
    if (!xcb_ewmh_get_client_list_reply(
            &d->ewmh,
            xcb_ewmh_get_client_list(&d->ewmh, d->screen_num),
            &reply, NULL)) {
        return 0;
    }

    int count = reply.windows_len;
    *wins = malloc(count * sizeof(IsdeWindow));
    if (*wins) {
        memcpy(*wins, reply.windows, count * sizeof(xcb_window_t));
    } else {
        count = 0;
    }
    xcb_ewmh_get_windows_reply_wipe(&reply);
    return count;
}

static int get_client_list_stacking(IsdeDisplay *d, IsdeWindow **wins)
{
    xcb_ewmh_get_windows_reply_t reply;
    if (!xcb_ewmh_get_client_list_stacking_reply(
            &d->ewmh,
            xcb_ewmh_get_client_list_stacking(&d->ewmh, d->screen_num),
            &reply, NULL)) {
        return 0;
    }

    int count = reply.windows_len;
    *wins = malloc(count * sizeof(IsdeWindow));
    if (*wins) {
        memcpy(*wins, reply.windows, count * sizeof(xcb_window_t));
    } else {
        count = 0;
    }
    xcb_ewmh_get_windows_reply_wipe(&reply);
    return count;
}

static IsdeAtom get_window_type(IsdeDisplay *d, IsdeWindow win)
{
    xcb_ewmh_get_atoms_reply_t reply;
    if (!xcb_ewmh_get_wm_window_type_reply(
            &d->ewmh,
            xcb_ewmh_get_wm_window_type(&d->ewmh, win),
            &reply, NULL)) {
        return XCB_ATOM_NONE;
    }

    xcb_atom_t type = reply.atoms_len > 0 ? reply.atoms[0] : XCB_ATOM_NONE;
    xcb_ewmh_get_atoms_reply_wipe(&reply);
    return type;
}

static int get_wm_class(IsdeDisplay *d, IsdeWindow win,
                        char **instance_out, char **class_out)
{
    xcb_icccm_get_wm_class_reply_t reply;
    if (!xcb_icccm_get_wm_class_reply(
            d->conn,
            xcb_icccm_get_wm_class(d->conn, win),
            &reply, NULL)) {
        return 0;
    }

    *instance_out = strdup(reply.instance_name);
    *class_out = strdup(reply.class_name);
    xcb_icccm_get_wm_class_reply_wipe(&reply);
    return 1;
}

static uint32_t get_wm_desktop(IsdeDisplay *d, IsdeWindow win)
{
    uint32_t desk = 0xFFFFFFFF;
    xcb_ewmh_get_wm_desktop_reply(
        &d->ewmh,
        xcb_ewmh_get_wm_desktop(&d->ewmh, win),
        &desk, NULL);
    return desk;
}

static void request_active_window(IsdeDisplay *d, IsdeWindow win)
{
    xcb_ewmh_request_change_active_window(
        &d->ewmh, d->screen_num, win,
        XCB_EWMH_CLIENT_SOURCE_TYPE_OTHER,
        XCB_CURRENT_TIME, XCB_WINDOW_NONE);
    xcb_flush(d->conn);
}

static void request_close_window(IsdeDisplay *d, IsdeWindow win)
{
    xcb_ewmh_request_close_window(
        &d->ewmh, d->screen_num, win,
        XCB_CURRENT_TIME,
        XCB_EWMH_CLIENT_SOURCE_TYPE_OTHER);
    xcb_flush(d->conn);
}

static int get_workarea(IsdeDisplay *d, int *x, int *y, int *w, int *h)
{
    xcb_ewmh_get_workarea_reply_t reply;
    if (xcb_ewmh_get_workarea_reply(
            &d->ewmh,
            xcb_ewmh_get_workarea(&d->ewmh, d->screen_num),
            &reply, NULL)) {
        uint32_t desk = 0;
        xcb_ewmh_get_current_desktop_reply(
            &d->ewmh,
            xcb_ewmh_get_current_desktop(&d->ewmh, d->screen_num),
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
    *w = d->screen->width_in_pixels;
    *h = d->screen->height_in_pixels;
    return 1;
}

static void request_current_desktop(IsdeDisplay *d, uint32_t desktop)
{
    xcb_ewmh_request_change_current_desktop(&d->ewmh, d->screen_num,
                                            desktop, XCB_CURRENT_TIME);
    xcb_flush(d->conn);
}

static void request_wm_desktop(IsdeDisplay *d, IsdeWindow win, uint32_t desktop)
{
    xcb_ewmh_request_change_wm_desktop(&d->ewmh, d->screen_num, win,
                                       desktop,
                                       XCB_EWMH_CLIENT_SOURCE_TYPE_OTHER);
    xcb_flush(d->conn);
}

static void set_desktop_layout(IsdeDisplay *d, int orientation,
                               int cols, int rows, int starting_corner)
{
    xcb_ewmh_set_desktop_layout(&d->ewmh, d->screen_num,
                                orientation, cols, rows, starting_corner);
    xcb_flush(d->conn);
}

static int get_desktop_layout(IsdeDisplay *d, int *orientation,
                              int *cols, int *rows, int *starting_corner)
{
    xcb_ewmh_get_desktop_layout_reply_t layout;
    if (!xcb_ewmh_get_desktop_layout_reply(
            &d->ewmh,
            xcb_ewmh_get_desktop_layout(&d->ewmh, d->screen_num),
            &layout, NULL)) {
        return 0;
    }
    *orientation = layout.orientation;
    *cols = layout.columns;
    *rows = layout.rows;
    *starting_corner = layout.starting_corner;
    return 1;
}

const IsdePlatformEwmhOps isde_x11_ewmh_ops = {
    .set_supported            = set_supported,
    .set_client_list          = set_client_list,
    .set_client_list_stacking = set_client_list_stacking,
    .set_active_window        = set_active_window,
    .set_number_of_desktops   = set_number_of_desktops,
    .set_current_desktop      = set_current_desktop,
    .set_wm_name              = set_wm_name,
    .get_active_window        = get_active_window,
    .get_current_desktop      = get_current_desktop,
    .get_number_of_desktops   = get_number_of_desktops,
    .get_client_list          = get_client_list,
    .get_client_list_stacking = get_client_list_stacking,
    .get_window_type          = get_window_type,
    .get_wm_class             = get_wm_class,
    .get_workarea             = get_workarea,
    .get_wm_desktop           = get_wm_desktop,
    .request_active_window    = request_active_window,
    .request_close_window     = request_close_window,
    .request_current_desktop  = request_current_desktop,
    .request_wm_desktop       = request_wm_desktop,
    .set_desktop_layout       = set_desktop_layout,
    .get_desktop_layout       = get_desktop_layout,
};

/* Public convenience: clamp w/h to the current workarea. (Was a free function
   in isde-ewmh.h; kept for settings/fm which size their initial window.) */
void isde_clamp_to_workarea(IsdeDisplay *d, int *w, int *h)
{
    int wx, wy, ww, wh;
    if (get_workarea(d, &wx, &wy, &ww, &wh)) {
        if (*w > ww) { *w = ww; }
        if (*h > wh) { *h = wh; }
    }
}
