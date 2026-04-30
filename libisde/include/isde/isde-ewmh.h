/*
 * isde-ewmh.h — EWMH / ICCCM atom cache and helpers
 *
 * Provides a pre-interned atom table and convenience functions for
 * reading/writing EWMH and ICCCM properties on windows.
 */
#ifndef ISDE_EWMH_H
#define ISDE_EWMH_H

#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IsdeEwmh IsdeEwmh;

/* Initialize the EWMH atom cache on the given connection.
 * Returns NULL on failure. */
IsdeEwmh *isde_ewmh_init(xcb_connection_t *conn, int screen);
void      isde_ewmh_free(IsdeEwmh *e);

/* Access the underlying xcb_ewmh_connection_t (for direct xcb-ewmh calls). */
xcb_ewmh_connection_t *isde_ewmh_connection(IsdeEwmh *e);
xcb_connection_t      *isde_ewmh_xcb(IsdeEwmh *e);
xcb_screen_t          *isde_ewmh_screen(IsdeEwmh *e);
xcb_window_t           isde_ewmh_root(IsdeEwmh *e);

/* Frequently-used atom helpers */
int isde_ewmh_set_supported(IsdeEwmh *e, xcb_atom_t *atoms, int count);
int isde_ewmh_set_client_list(IsdeEwmh *e, xcb_window_t *wins, int count);
int isde_ewmh_set_active_window(IsdeEwmh *e, xcb_window_t win);
int isde_ewmh_set_number_of_desktops(IsdeEwmh *e, uint32_t n);
int isde_ewmh_set_current_desktop(IsdeEwmh *e, uint32_t desk);
int isde_ewmh_set_wm_name(IsdeEwmh *e, xcb_window_t win,
                           const char *name);

/* Read helpers */
xcb_window_t  isde_ewmh_get_active_window(IsdeEwmh *e);
uint32_t      isde_ewmh_get_current_desktop(IsdeEwmh *e);
uint32_t      isde_ewmh_get_number_of_desktops(IsdeEwmh *e);

/* Get _NET_CLIENT_LIST.  Caller must free() the returned array.
 * Returns the count; *wins is set to the window array. */
int isde_ewmh_get_client_list(IsdeEwmh *e, xcb_window_t **wins);

/* Get _NET_WM_WINDOW_TYPE for a window.  Returns the first type atom. */
xcb_atom_t isde_ewmh_get_window_type(IsdeEwmh *e, xcb_window_t win);

/* Get WM_CLASS (ICCCM).  Caller must free class_out and instance_out. */
int isde_ewmh_get_wm_class(IsdeEwmh *e, xcb_window_t win,
                            char **instance_out, char **class_out);

/* Get _NET_WORKAREA for the current desktop.
 * Returns 1 on success (x/y/w/h filled in), 0 on failure.
 * Falls back to screen geometry if _NET_WORKAREA is not set. */
int isde_ewmh_get_workarea(IsdeEwmh *e, int *x, int *y, int *w, int *h);

/* Clamp width/height so the window fits within the working area.
 * Convenience wrapper: initializes a temporary IsdeEwmh, queries workarea,
 * clamps *w and *h, and cleans up. Safe to call from any app. */
void isde_clamp_to_workarea(xcb_connection_t *conn, int screen,
                            int *w, int *h);

/* Get _NET_WM_DESKTOP for a window.  Returns the desktop index,
 * or 0xFFFFFFFF if the window is sticky / property not set. */
uint32_t isde_ewmh_get_wm_desktop(IsdeEwmh *e, xcb_window_t win);

/* Send a _NET_ACTIVE_WINDOW client message to the root. */
void isde_ewmh_request_active_window(IsdeEwmh *e, xcb_window_t win);

/* Send a _NET_CLOSE_WINDOW client message. */
void isde_ewmh_request_close_window(IsdeEwmh *e, xcb_window_t win);

#ifdef __cplusplus
}
#endif

#endif /* ISDE_EWMH_H */
