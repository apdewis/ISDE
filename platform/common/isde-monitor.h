/*
 * isde-monitor.h — platform-independent monitor geometry abstraction.
 *
 * Provides a vtable for querying monitor information.  Backends (e.g.
 * xcb-randr) implement the ops and register via isde_monitor_probe().
 */
#ifndef _ISDE_MONITOR_H
#define _ISDE_MONITOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t  x, y;
    uint16_t width, height;
} IsdeMonitor;

typedef enum {
    ISDE_MONITOR_BACKEND_NONE = 0,
    ISDE_MONITOR_BACKEND_XCB_RANDR,
} IsdeMonitorBackend;

/*
 * All ops receive an opaque context pointer (ctx) whose meaning is
 * backend-specific.  For xcb-randr this is an IsdeMonitorXcbCtx*.
 */
typedef struct {
    IsdeMonitorBackend backend;

    /* Get the primary monitor geometry.
     * Returns 1 if a real primary was found, 0 on fallback to full screen. */
    int  (*get_primary)(void *ctx, IsdeMonitor *out);

    /* Get the monitor containing the point (px, py).
     * Returns 1 if found, 0 on fallback. */
    int  (*monitor_at)(void *ctx, int px, int py, IsdeMonitor *out);

    /* Query all active monitors.  Allocates *out; caller must free().
     * Returns the count (0 on failure). */
    int  (*get_monitors)(void *ctx, IsdeMonitor **out);
} IsdeMonitorOps;

/*
 * Backend probing is platform-specific.  Include the appropriate
 * backend header (e.g. isde-monitor-xcb.h) and call its probe function.
 */

#ifdef __cplusplus
}
#endif

#endif  /* _ISDE_MONITOR_H */
