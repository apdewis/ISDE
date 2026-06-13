#define _POSIX_C_SOURCE 200809L
/*
 * isde-tray.c — helpers for system tray applet popups
 */
#include "isde/isde-tray.h"
#include "isde/isde-ewmh.h"
#include "isde/isde-randr.h"

#include <ISW/IntrinsicP.h>
#include <ISW/ISWRender.h>
#include <xcb/xcb.h>
#include <math.h>
#include <stdlib.h>

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
    int icon_x = reply->dst_x;
    int icon_y = reply->dst_y;
    free(reply);

    Dimension w  = popup_shell->core.width;
    Dimension h  = popup_shell->core.height;
    Dimension bw = popup_shell->core.border_width;
    int total_w  = (int)lrint((w + 2 * bw) * sf);
    int total_h  = (int)lrint((h + 2 * bw) * sf);

    /* Find which monitor the tray icon is on */
    IsdeMonitor mon;
    //isde_randr_monitor_at(conn, root, scr, icon_x, icon_y, &mon);

    /* Panel top edge: use workarea if available, else monitor bottom */
    int panel_top = mon.y + mon.height;

    IsdeEwmh *ewmh = isde_ewmh_init(conn, 0);
    if (ewmh) {
        int wa_x, wa_y, wa_w, wa_h;
        if (isde_ewmh_get_workarea(ewmh, &wa_x, &wa_y, &wa_w, &wa_h)) {
            int wa_bottom = wa_y + wa_h;
            int mon_bottom = mon.y + mon.height;
            if (wa_bottom < mon_bottom)
                panel_top = wa_bottom;
        }
        isde_ewmh_free(ewmh);
    }

    int x = icon_x;
    int y = panel_top - total_h;

    /* Clamp to monitor bounds */
    if (x + total_w > mon.x + mon.width)
        x = mon.x + mon.width - total_w;
    if (x < mon.x) x = mon.x;
    if (y < mon.y) y = mon.y;

    /* Convert physical to logical for IswConfigureWidget */
    IswConfigureWidget(popup_shell,
                       (int)(x / sf), (int)(y / sf), w, h, bw);
}
