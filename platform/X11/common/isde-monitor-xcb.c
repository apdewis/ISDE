/*
 * isde-monitor-xcb.c — xcb-randr backend for IsdeMonitorOps.
 */
#include "isde-monitor-xcb.h"
#include "randr.h"

static int mon_xcb_get_primary(void *ctx, IsdeMonitor *out)
{
    IsdeMonitorXcbCtx *c = (IsdeMonitorXcbCtx *)ctx;
    return isde_randr_primary(c->conn, c->root, c->screen, out);
}

static int mon_xcb_monitor_at(void *ctx, int px, int py, IsdeMonitor *out)
{
    IsdeMonitorXcbCtx *c = (IsdeMonitorXcbCtx *)ctx;
    return isde_randr_monitor_at(c->conn, c->root, c->screen, px, py, out);
}

static int mon_xcb_get_monitors(void *ctx, IsdeMonitor **out)
{
    IsdeMonitorXcbCtx *c = (IsdeMonitorXcbCtx *)ctx;
    return isde_randr_monitors(c->conn, c->root, out);
}

static const IsdeMonitorOps xcb_randr_ops = {
    .backend      = ISDE_MONITOR_BACKEND_XCB_RANDR,
    .get_primary  = mon_xcb_get_primary,
    .monitor_at   = mon_xcb_monitor_at,
    .get_monitors = mon_xcb_get_monitors,
};

const IsdeMonitorOps *isde_monitor_xcb_probe(xcb_connection_t *conn)
{
    const xcb_query_extension_reply_t *ext =
        xcb_get_extension_data(conn, &xcb_randr_id);
    if (!ext || !ext->present)
        return NULL;
    return &xcb_randr_ops;
}

const IsdeMonitorOps *isde_monitor_xcb_randr_ops(void)
{
    return &xcb_randr_ops;
}
