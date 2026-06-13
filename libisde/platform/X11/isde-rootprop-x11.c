#define _POSIX_C_SOURCE 200809L
/*
 * isde-rootprop-x11.c — X11 backend: root-window property publishing.
 *
 * The RESOURCE_MANAGER write absorbed from the former isde-theme.c
 * isde_theme_set_resource_manager(). Theme code still builds the resource
 * string; this op transfers it to the root property.
 */
#include "isde-platform-x11.h"

#include <stdlib.h>
#include <string.h>

static void set_resource_manager(IsdeDisplay *d, const char *rdb, size_t length)
{
    xcb_window_t root = d->screen->root;

    xcb_intern_atom_cookie_t ck =
        xcb_intern_atom(d->conn, 0, (uint16_t)strlen("RESOURCE_MANAGER"),
                        "RESOURCE_MANAGER");
    xcb_intern_atom_reply_t *reply =
        xcb_intern_atom_reply(d->conn, ck, NULL);
    if (reply) {
        xcb_change_property(d->conn, XCB_PROP_MODE_REPLACE, root,
                            reply->atom, XCB_ATOM_STRING, 8,
                            (uint32_t)length, rdb);
        xcb_flush(d->conn);
        free(reply);
    }
}

const IsdePlatformRootOps isde_x11_root_ops = {
    .set_resource_manager = set_resource_manager,
};
