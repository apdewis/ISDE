#define _POSIX_C_SOURCE 200809L
/*
 * isde-dpms.c — DPMS timeout get/set via xcb-dpms
 */
#include "isde/isde-dpms.h"

#include <xcb/dpms.h>
#include <stdlib.h>

int isde_dpms_get_timeouts(xcb_connection_t *conn,
                           int *standby, int *suspend, int *off)
{
    xcb_dpms_get_timeouts_cookie_t cookie = xcb_dpms_get_timeouts(conn);
    xcb_dpms_get_timeouts_reply_t *reply =
        xcb_dpms_get_timeouts_reply(conn, cookie, NULL);
    if (!reply) {
        return -1;
    }

    if (standby) { *standby = reply->standby_timeout; }
    if (suspend) { *suspend = reply->suspend_timeout; }
    if (off)     { *off     = reply->off_timeout; }

    free(reply);
    return 0;
}

int isde_dpms_set_timeouts(xcb_connection_t *conn,
                           int standby, int suspend, int off)
{
    if (standby > 0 || suspend > 0 || off > 0) {
        xcb_dpms_enable(conn);
    } else {
        xcb_dpms_disable(conn);
    }

    xcb_dpms_set_timeouts(conn, standby, suspend, off);
    xcb_flush(conn);
    return 0;
}
