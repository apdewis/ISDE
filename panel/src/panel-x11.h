/*
 * panel-x11.h — panel's X11-specific operations.
 *
 * The raw xcb / RandR / cursor / EWMH-window code from panel.c lives in
 * panel-x11.c. Existing function names are unchanged; only their location moved.
 * panel_init_display / panel_setup_dock_window / panel_update_strut wrap blocks
 * that were inline in panel_init() / panel_reconfigure().
 */
#ifndef PANEL_X11_H
#define PANEL_X11_H

#include "panel.h"

/* Acquire conn/screen/root/screen_num from the toplevel and intern the WM_NAME
 * atoms. Returns 0 on success, -1 if the connection is in error. */
int   panel_init_display(Panel *p);

void  query_primary_monitor(Panel *p);

/* After the shell is realized: clear override-redirect, set dock type + strut,
 * select root events, register the root drawable + IPC handler, subscribe to
 * RandR screen-change events. */
void  panel_setup_dock_window(Panel *p);

/* Set _NET_WM_STRUT_PARTIAL for the current monitor geometry, then flush. */
void  panel_update_strut(Panel *p);

Pixel panel_color_pixel(Panel *p, unsigned int rgb);

/* Release any keyboard/pointer grab held by a popup, then flush. */
void  panel_ungrab_popup(Panel *p);

void  launch_cursor_init(Panel *p);
void  set_panel_cursor(Panel *p, xcb_cursor_t cursor);

#endif /* PANEL_X11_H */
