/*
 * isde-randr.h — shared RandR helpers: monitor geometry, CRTC allocation
 */
#ifndef ISDE_RANDR_H
#define ISDE_RANDR_H

#include <xcb/xcb.h>
#include <xcb/randr.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t  x, y;
    uint16_t width, height;
} IsdeMonitor;

/* Compute refresh rate (Hz) from a RandR mode info struct. */
double isde_randr_refresh(xcb_randr_mode_info_t *mi);

/* Get the primary monitor's geometry in physical pixels.
 * Falls back to first connected output with a CRTC, then full screen.
 * Returns 1 if a monitor was found, 0 on fallback to full screen. */
int isde_randr_primary(xcb_connection_t *conn, xcb_window_t root,
                       xcb_screen_t *scr, IsdeMonitor *out);

/* Find the monitor containing (px, py) in physical pixels.
 * Returns 1 if found, 0 on fallback (out set to first active monitor
 * or full screen). */
int isde_randr_monitor_at(xcb_connection_t *conn, xcb_window_t root,
                          xcb_screen_t *scr, int px, int py,
                          IsdeMonitor *out);

/* Find a free CRTC for the given output (one with mode==NONE and
 * num_outputs==0 from the output's possible CRTC list).
 * Returns the CRTC id or XCB_NONE if all are busy. */
xcb_randr_crtc_t isde_randr_find_free_crtc(xcb_connection_t *conn,
                                            xcb_randr_get_output_info_reply_t *oinfo,
                                            xcb_timestamp_t cfg_ts);

/* Query all active monitors (CRTCs with connected outputs).
 * Allocates *out; caller must free().  Returns count. */
int isde_randr_monitors(xcb_connection_t *conn, xcb_window_t root,
                        IsdeMonitor **out);

#ifdef __cplusplus
}
#endif

#endif /* ISDE_RANDR_H */
