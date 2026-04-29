#define _POSIX_C_SOURCE 200809L
/*
 * isde-tray.c — helpers for system tray applet popups
 */
#include "isde/isde-tray.h"
#include "isde/isde-ewmh.h"

#include <ISW/IntrinsicP.h>
#include <ISW/ISWRender.h>
#include <xcb/xcb.h>
#include <stdlib.h>

void isde_tray_position_popup(Widget toplevel, IswTrayIcon tray_icon,
                              Widget popup_shell)
{
    if (!tray_icon || !popup_shell)
        return;

    xcb_connection_t *conn = IswDisplay(toplevel);
    xcb_window_t icon_win = IswTrayIconGetWindow(tray_icon);
    xcb_window_t root = IswScreen(toplevel)->root;

    xcb_translate_coordinates_cookie_t cookie =
        xcb_translate_coordinates(conn, icon_win, root, 0, 0);
    xcb_translate_coordinates_reply_t *reply =
        xcb_translate_coordinates_reply(conn, cookie, NULL);
    if (!reply)
        return;

    double sf = ISWScaleFactor(toplevel);
    int icon_x = (int)(reply->dst_x / sf);
    free(reply);

    Dimension w  = popup_shell->core.width;
    Dimension h  = popup_shell->core.height;
    Dimension bw = popup_shell->core.border_width;
    int total_w  = (int)(w + 2 * bw);
    int total_h  = (int)(h + 2 * bw);
    int scr_w    = (int)(IswScreen(toplevel)->width_in_pixels / sf);

    /* Workarea bottom edge = panel top edge (logical pixels) */
    int panel_top = (int)(IswScreen(toplevel)->height_in_pixels / sf);

    IsdeEwmh *ewmh = isde_ewmh_init(conn, 0);
    if (ewmh) {
        int wa_x, wa_y, wa_w, wa_h;
        if (isde_ewmh_get_workarea(ewmh, &wa_x, &wa_y, &wa_w, &wa_h))
            panel_top = wa_y + wa_h;
        isde_ewmh_free(ewmh);
    }

    int x = icon_x;
    int y = panel_top - total_h;

    if (x + total_w > scr_w)
        x = scr_w - total_w;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    IswConfigureWidget(popup_shell, x, y, w, h, bw);
}
