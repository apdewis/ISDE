/*
 * isde-monitor-xcb.h — xcb-randr monitor backend context.
 */
#ifndef ISDE_MONITOR_XCB_H
#define ISDE_MONITOR_XCB_H

#include "../../common/isde-monitor.h"
#include <xcb/xcb.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    xcb_connection_t *conn;
    xcb_window_t      root;
    xcb_screen_t     *screen;
} IsdeMonitorXcbCtx;

/* Probe whether xcb-randr is available on this connection.
 * Returns the ops table if the RandR extension is present, NULL otherwise. */
const IsdeMonitorOps *isde_monitor_xcb_probe(xcb_connection_t *conn);

/* Return the xcb-randr ops table unconditionally (for callers that
 * already know randr is available). */
const IsdeMonitorOps *isde_monitor_xcb_randr_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* ISDE_MONITOR_XCB_H */
