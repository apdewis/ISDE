/*
 * isde-dpms.h — DPMS timeout get/set via xcb-dpms
 */
#ifndef ISDE_DPMS_H
#define ISDE_DPMS_H

#include <xcb/xcb.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Get current DPMS timeouts (in seconds).  Returns 0 on success. */
int isde_dpms_get_timeouts(xcb_connection_t *conn,
                           int *standby, int *suspend, int *off);

/* Set DPMS timeouts (in seconds).  Enables DPMS if any timeout > 0.
 * Returns 0 on success. */
int isde_dpms_set_timeouts(xcb_connection_t *conn,
                           int standby, int suspend, int off);

#ifdef __cplusplus
}
#endif

#endif /* ISDE_DPMS_H */
