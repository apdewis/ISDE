#define _POSIX_C_SOURCE 200809L
/*
 * isde-platform-x11.c — X11 backend: display lifecycle + ops table.
 *
 * The platform layer owns the connection. isde_platform_open() opens it, interns
 * the EWMH atom cache, the ISDE IPC atom and the startup-notification atoms, and
 * resolves the default screen. Every backend TU works off this IsdeDisplay.
 */
#include "isde-platform-x11.h"

#include <stdlib.h>
#include <string.h>
#include <xcb/xcb_aux.h>

static const IsdePlatformOps g_x11_ops = {
    .ewmh           = &isde_x11_ewmh_ops,
    .display        = &isde_x11_display_ops,
    .display_config = &isde_x11_display_config_ops,
    .power          = &isde_x11_power_ops,
    .ipc            = &isde_x11_ipc_ops,
    .startup        = &isde_x11_startup_ops,
    .root           = &isde_x11_root_ops,
};

const IsdePlatformOps *isde_platform(void)
{
    return &g_x11_ops;
}

static xcb_atom_t intern(xcb_connection_t *conn, const char *name)
{
    xcb_intern_atom_cookie_t ck =
        xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(conn, ck, NULL);
    if (!r) {
        return XCB_ATOM_NONE;
    }
    xcb_atom_t a = r->atom;
    free(r);
    return a;
}

IsdeDisplay *isde_platform_open(const char *display_name)
{
    IsdeDisplay *d = calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }

    d->conn = xcb_connect(display_name, &d->screen_num);
    if (xcb_connection_has_error(d->conn)) {
        xcb_disconnect(d->conn);
        free(d);
        return NULL;
    }

    xcb_intern_atom_cookie_t *cookies = xcb_ewmh_init_atoms(d->conn, &d->ewmh);
    if (!cookies ||
        !xcb_ewmh_init_atoms_replies(&d->ewmh, cookies, NULL)) {
        xcb_disconnect(d->conn);
        free(d);
        return NULL;
    }

    d->screen = xcb_aux_get_screen(d->conn, d->screen_num);
    if (!d->screen) {
        xcb_ewmh_connection_wipe(&d->ewmh);
        xcb_disconnect(d->conn);
        free(d);
        return NULL;
    }

    d->ipc_atom           = intern(d->conn, ISDE_IPC_ATOM);
    d->startup_info_begin = intern(d->conn, "_NET_STARTUP_INFO_BEGIN");
    d->startup_info       = intern(d->conn, "_NET_STARTUP_INFO");
    d->startup_id         = intern(d->conn, "_NET_STARTUP_ID");

    return d;
}

void isde_display_close(IsdeDisplay *d)
{
    if (!d) {
        return;
    }
    xcb_ewmh_connection_wipe(&d->ewmh);
    xcb_disconnect(d->conn);
    free(d);
}

int isde_display_event_fd(IsdeDisplay *d)
{
    return xcb_get_file_descriptor(d->conn);
}

IsdeEvent *isde_display_poll_event(IsdeDisplay *d)
{
    xcb_generic_event_t *xev = xcb_poll_for_event(d->conn);
    if (!xev) {
        return NULL;
    }
    /* Copy into an IsdeEvent (xcb_generic_event_t by value as first member). */
    IsdeEvent *ev = malloc(sizeof(*ev));
    if (ev) {
        ev->native = *xev;
    }
    free(xev);
    return ev;
}

void isde_display_flush(IsdeDisplay *d)
{
    xcb_flush(d->conn);
}

IsdeWindow isde_display_root(IsdeDisplay *d)
{
    return d->screen->root;
}
