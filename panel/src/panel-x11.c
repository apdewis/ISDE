#define _POSIX_C_SOURCE 200809L
/*
 * panel-x11.c — panel's X11-specific operations.
 *
 * Holds the raw xcb / RandR / cursor / EWMH-window requests the panel makes,
 * moved out of panel.c. Function names are unchanged from panel.c.
 */
#include "panel-x11.h"
#include "../../platform/common/dbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_cursor.h>
#include <xcb/randr.h>

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

int panel_init_platform(Panel *p) {
    /* Query primary monitor geometry */
    query_primary_monitor(p);

    /* Clear override-redirect, set dock type + strut, select root events,
     * register the IPC handler, subscribe to RandR — all before mapping. */
    panel_setup_dock_window(p);
    pager_init(p);
}

int panel_init_display(Panel *p)
{
    p->conn = IswDisplay(p->toplevel);
    if (xcb_connection_has_error(p->conn)) {
        return -1;
    }

    p->screen = IswScreen(p->toplevel);
    p->root = p->screen->root;

    /* Find screen number */
    p->screen_num = 0;
    xcb_screen_iterator_t si = xcb_setup_roots_iterator(
        xcb_get_setup(p->conn));
    for (int i = 0; si.rem; xcb_screen_next(&si), i++) {
        if (si.data->root == p->root) {
            p->screen_num = i;
            break;
        }
    }

    p->atom_net_wm_name         = intern(p->conn, "_NET_WM_NAME");
    p->atom_net_wm_visible_name = intern(p->conn, "_NET_WM_VISIBLE_NAME");
    p->atom_wm_name             = intern(p->conn, "WM_NAME");
    return 0;
}

/* Query primary monitor geometry via RandR.
 * Falls back to full screen if RandR is unavailable or no primary is set. */
void query_primary_monitor(Panel *p)
{
    /* Default to full screen */
    p->mon_x = 0;
    p->mon_y = 0;
    p->mon_w = p->screen->width_in_pixels;
    p->mon_h = p->screen->height_in_pixels;

    xcb_randr_get_output_primary_reply_t *primary =
        xcb_randr_get_output_primary_reply(p->conn,
            xcb_randr_get_output_primary(p->conn, p->root), NULL);
    if (!primary) {
        return;
    }

    xcb_randr_output_t pout = primary->output;
    free(primary);

    if (pout == XCB_NONE) {
        return;
    }

    xcb_randr_get_output_info_reply_t *oinfo =
        xcb_randr_get_output_info_reply(p->conn,
            xcb_randr_get_output_info(p->conn, pout, XCB_CURRENT_TIME),
            NULL);
    if (!oinfo) {
        return;
    }

    xcb_randr_crtc_t crtc = oinfo->crtc;
    free(oinfo);

    if (crtc == XCB_NONE) {
        return;
    }

    xcb_randr_get_crtc_info_reply_t *cinfo =
        xcb_randr_get_crtc_info_reply(p->conn,
            xcb_randr_get_crtc_info(p->conn, crtc, XCB_CURRENT_TIME),
            NULL);
    if (!cinfo) {
        return;
    }

    p->mon_x = cinfo->x;
    p->mon_y = cinfo->y;
    p->mon_w = cinfo->width;
    p->mon_h = cinfo->height;
    free(cinfo);
}

void panel_update_strut(Panel *p)
{
    xcb_ewmh_connection_t *ewmh = isde_ewmh_connection(p->ewmh);
    xcb_ewmh_wm_strut_partial_t strut;
    memset(&strut, 0, sizeof(strut));
    strut.bottom = p->phys_panel_h;
    strut.bottom_start_x = p->mon_x;
    strut.bottom_end_x = p->mon_x + p->mon_w - 1;
    xcb_ewmh_set_wm_strut_partial(ewmh, IswWindow(p->shell), strut);
    xcb_flush(p->conn);
}

void panel_setup_dock_window(Panel *p)
{
    xcb_ewmh_connection_t *ewmh = isde_ewmh_connection(p->ewmh);
    xcb_window_t panel_win = IswWindow(p->shell);

    /* Clear override_redirect so the WM receives MapRequest */
    uint32_t ov = 0;
    xcb_change_window_attributes(p->conn, panel_win,
                                 XCB_CW_OVERRIDE_REDIRECT, &ov);

    /* Set _NET_WM_WINDOW_TYPE_DOCK and strut before mapping so the WM
       can read them when it receives MapRequest */
    xcb_atom_t dock_type = ewmh->_NET_WM_WINDOW_TYPE_DOCK;
    xcb_ewmh_set_wm_window_type(ewmh, panel_win, 1, &dock_type);

    xcb_ewmh_wm_strut_partial_t strut;
    memset(&strut, 0, sizeof(strut));
    strut.bottom = p->phys_panel_h;
    strut.bottom_start_x = p->mon_x;
    strut.bottom_end_x = p->mon_x + p->mon_w - 1;
    xcb_ewmh_set_wm_strut_partial(ewmh, panel_win, strut);

    /* Watch root for:
     *   PROPERTY_CHANGE — client list updates
     *   STRUCTURE_NOTIFY — required for ClientMessages sent to root with
     *                      SUBSTRUCTURE_REDIRECT|STRUCTURE_NOTIFY mask
     *                      (isde_ipc_send uses that mask). */
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE |
                    XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(p->conn, p->root,
                                 XCB_CW_EVENT_MASK, &mask);

    /* Route root-window events to the shell so raw handlers can receive
     * IPC ClientMessages (ISW drops unmatched root events otherwise). */
    IswRegisterDrawable(p->conn, p->root, p->shell);
    IswAddRawEventHandler(p->shell,
                         XCB_EVENT_MASK_STRUCTURE_NOTIFY,
                         True,  /* nonmaskable — needed for ClientMessage */
                         panel_ipc_event_handler, p);

    /* Subscribe to RandR screen change events */
    xcb_randr_select_input(p->conn, p->root,
                           XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);

    xcb_flush(p->conn);
}

Pixel panel_color_pixel(Panel *p, unsigned int rgb)
{
    xcb_alloc_color_reply_t *reply = xcb_alloc_color_reply(
        p->conn,
        xcb_alloc_color(p->conn, p->screen->default_colormap,
                        ((rgb >> 16) & 0xFF) * 257,
                        ((rgb >> 8)  & 0xFF) * 257,
                        ( rgb        & 0xFF) * 257),
        NULL);
    if (!reply) {
        return p->screen->white_pixel;
    }
    Pixel px = reply->pixel;
    free(reply);
    return px;
}

void panel_ungrab_popup(Panel *p)
{
    xcb_ungrab_keyboard(p->conn, XCB_CURRENT_TIME);
    xcb_ungrab_pointer(p->conn, XCB_CURRENT_TIME);
    xcb_flush(p->conn);
}

void launch_cursor_init(Panel *p)
{
    if (p->cursor_watch) {
        return;
    }
    xcb_cursor_context_t *ctx;
    if (xcb_cursor_context_new(p->conn, p->screen, &ctx) < 0) {
        return;
    }
    p->cursor_watch = xcb_cursor_load_cursor(ctx, "watch");
    p->cursor_default = xcb_cursor_load_cursor(ctx, "left_ptr");
    xcb_cursor_context_free(ctx);
}

void set_panel_cursor(Panel *p, xcb_cursor_t cursor)
{
    if (!p->shell || !IswIsRealized(p->shell)) {
        return;
    }
    uint32_t vals[] = { cursor };
    xcb_change_window_attributes(p->conn, IswWindow(p->shell),
                                 XCB_CW_CURSOR, vals);
    xcb_flush(p->conn);
}
