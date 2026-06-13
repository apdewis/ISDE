#define _POSIX_C_SOURCE 200809L
/*
 * isde-power-x11.c — X11 backend: DPMS ops.
 *
 * Absorbs the body of the former isde-dpms.c, reshaped onto IsdeDisplay.
 */
#include "isde-platform-x11.h"

#include <stdlib.h>
#include <xcb/dpms.h>

static int get_timeouts(IsdeDisplay *d, int *standby, int *suspend, int *off)
{
    xcb_dpms_get_timeouts_reply_t *reply =
        xcb_dpms_get_timeouts_reply(d->conn,
            xcb_dpms_get_timeouts(d->conn), NULL);
    if (!reply) {
        return -1;
    }

    if (standby) { *standby = reply->standby_timeout; }
    if (suspend) { *suspend = reply->suspend_timeout; }
    if (off)     { *off     = reply->off_timeout; }

    free(reply);
    return 0;
}

static int set_timeouts(IsdeDisplay *d, int standby, int suspend, int off)
{
    if (standby > 0 || suspend > 0 || off > 0) {
        xcb_dpms_enable(d->conn);
    } else {
        xcb_dpms_disable(d->conn);
    }

    xcb_dpms_set_timeouts(d->conn, standby, suspend, off);
    xcb_flush(d->conn);
    return 0;
}

const IsdePlatformPowerOps isde_x11_power_ops = {
    .get_timeouts = get_timeouts,
    .set_timeouts = set_timeouts,
};
