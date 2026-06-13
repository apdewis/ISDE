#define _POSIX_C_SOURCE 200809L
/*
 * theme-x11.c — X11-specific theme publishing.
 *
 * The theme payload (colours, fonts, cursor) is built platform-agnostically by
 * isde_theme_build_resource_string() in platform/common/isde-theme.c.  Writing
 * it to the root window's RESOURCE_MANAGER atom is X11-specific and lives here.
 */
#include "isde-theme.h"

#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

void isde_theme_set_resource_manager(xcb_connection_t *conn, xcb_window_t root)
{
    if (!conn) return;

    char *rdb = isde_theme_build_resource_string();
    if (!rdb) return;

    xcb_intern_atom_cookie_t ck =
        xcb_intern_atom(conn, 0, strlen("RESOURCE_MANAGER"),
                        "RESOURCE_MANAGER");
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(conn, ck, NULL);
    if (reply) {
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root,
                            reply->atom, XCB_ATOM_STRING, 8,
                            (uint32_t)strlen(rdb), rdb);
        xcb_flush(conn);
        free(reply);
    }

    free(rdb);
}
