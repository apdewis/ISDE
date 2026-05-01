#define _POSIX_C_SOURCE 200809L
/*
 * isde-tray.c — helpers for system tray applet popups
 */
#include "isde/isde-tray.h"
#include "isde/isde-ewmh.h"

#include <ISW/IntrinsicP.h>
#include <ISW/ISWRender.h>
#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <stdlib.h>

/* Find the monitor containing (px, py) in physical pixels.
 * Returns the monitor rect in logical pixels via out params. */
static void monitor_for_point(xcb_connection_t *conn, xcb_window_t root,
                               double sf, int px, int py,
                               int *mx, int *my, int *mw, int *mh)
{
    *mx = 0;
    *my = 0;
    *mw = 0;
    *mh = 0;

    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, root), NULL);
    if (!res) return;

    xcb_timestamp_t ts = res->config_timestamp;
    xcb_randr_crtc_t *crtcs =
        xcb_randr_get_screen_resources_current_crtcs(res);
    int ncrtcs = xcb_randr_get_screen_resources_current_crtcs_length(res);

    for (int i = 0; i < ncrtcs; i++) {
        xcb_randr_get_crtc_info_reply_t *ci =
            xcb_randr_get_crtc_info_reply(conn,
                xcb_randr_get_crtc_info(conn, crtcs[i], ts), NULL);
        if (!ci) continue;
        if (ci->mode == XCB_NONE || ci->num_outputs == 0) {
            free(ci);
            continue;
        }

        if (px >= ci->x && px < ci->x + ci->width &&
            py >= ci->y && py < ci->y + ci->height) {
            *mx = (int)(ci->x / sf);
            *my = (int)(ci->y / sf);
            *mw = (int)(ci->width / sf);
            *mh = (int)(ci->height / sf);
            free(ci);
            free(res);
            return;
        }

        /* Keep first CRTC as fallback */
        if (*mw == 0) {
            *mx = (int)(ci->x / sf);
            *my = (int)(ci->y / sf);
            *mw = (int)(ci->width / sf);
            *mh = (int)(ci->height / sf);
        }
        free(ci);
    }

    free(res);
}

void isde_tray_position_popup(Widget toplevel, IswTrayIcon tray_icon,
                              Widget popup_shell)
{
    if (!tray_icon || !popup_shell)
        return;

    xcb_connection_t *conn = IswDisplay(toplevel);
    xcb_window_t icon_win = IswTrayIconGetWindow(tray_icon);
    xcb_screen_t *scr = IswScreen(toplevel);
    xcb_window_t root = scr->root;

    xcb_translate_coordinates_cookie_t cookie =
        xcb_translate_coordinates(conn, icon_win, root, 0, 0);
    xcb_translate_coordinates_reply_t *reply =
        xcb_translate_coordinates_reply(conn, cookie, NULL);
    if (!reply)
        return;

    double sf = ISWScaleFactor(toplevel);
    int icon_phys_x = reply->dst_x;
    int icon_phys_y = reply->dst_y;
    int icon_x = (int)(icon_phys_x / sf);
    free(reply);

    Dimension w  = popup_shell->core.width;
    Dimension h  = popup_shell->core.height;
    Dimension bw = popup_shell->core.border_width;
    int total_w  = (int)(w + 2 * bw);
    int total_h  = (int)(h + 2 * bw);

    /* Find which monitor the tray icon is on */
    int mon_x, mon_y, mon_w, mon_h;
    monitor_for_point(conn, root, sf, icon_phys_x, icon_phys_y,
                      &mon_x, &mon_y, &mon_w, &mon_h);

    if (mon_w == 0) {
        mon_x = 0;
        mon_y = 0;
        mon_w = (int)(scr->width_in_pixels / sf);
        mon_h = (int)(scr->height_in_pixels / sf);
    }

    /* Panel top edge: use workarea if available, else monitor bottom */
    int panel_top = mon_y + mon_h;

    IsdeEwmh *ewmh = isde_ewmh_init(conn, 0);
    if (ewmh) {
        int wa_x, wa_y, wa_w, wa_h;
        if (isde_ewmh_get_workarea(ewmh, &wa_x, &wa_y, &wa_w, &wa_h)) {
            /* Intersect workarea bottom with this monitor */
            int wa_bottom = wa_y + wa_h;
            int mon_bottom = mon_y + mon_h;
            if (wa_bottom < mon_bottom)
                panel_top = wa_bottom;
        }
        isde_ewmh_free(ewmh);
    }

    int x = icon_x;
    int y = panel_top - total_h;

    /* Clamp to monitor bounds */
    if (x + total_w > mon_x + mon_w)
        x = mon_x + mon_w - total_w;
    if (x < mon_x) x = mon_x;
    if (y < mon_y) y = mon_y;

    IswConfigureWidget(popup_shell, x, y, w, h, bw);
}
