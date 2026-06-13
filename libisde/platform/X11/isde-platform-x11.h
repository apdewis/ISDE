/*
 * isde-platform-x11.h — X11 backend internals (private to libisde/platform/X11).
 *
 * Shared declarations across the X11 backend TUs: the concrete IsdeDisplay /
 * IsdeEvent layout and the per-category ops sub-tables each TU defines. Not
 * installed; not part of the public ISDE API.
 */
#ifndef ISDE_PLATFORM_X11_H
#define ISDE_PLATFORM_X11_H

#include "isde/isde-platform.h"

#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>

/* The X11 IsdeDisplay: the layer owns the connection. */
struct IsdeDisplay {
    xcb_connection_t      *conn;
    int                    screen_num;
    xcb_screen_t          *screen;       /* default screen */
    xcb_ewmh_connection_t  ewmh;         /* EWMH atom cache */
    xcb_atom_t             ipc_atom;     /* _ISDE_COMMAND */
    xcb_atom_t             startup_info_begin;
    xcb_atom_t             startup_info;
    xcb_atom_t             startup_id;
    long                   startup_seq;  /* startup-id sequence counter */
};

/* The X11 IsdeEvent holds the native xcb event by value as its first member, so
   the whole thing is one allocation freed with a single free(). The backend
   reads ev->native back as an xcb_generic_event_t*. */
struct IsdeEvent {
    xcb_generic_event_t native;
};

/* Per-category ops tables, defined one per backend TU. */
extern const IsdePlatformEwmhOps          isde_x11_ewmh_ops;
extern const IsdePlatformDisplayOps       isde_x11_display_ops;
extern const IsdePlatformDisplayConfigOps isde_x11_display_config_ops;
extern const IsdePlatformPowerOps         isde_x11_power_ops;
extern const IsdePlatformIpcOps           isde_x11_ipc_ops;
extern const IsdePlatformStartupOps       isde_x11_startup_ops;
extern const IsdePlatformRootOps          isde_x11_root_ops;

#endif /* ISDE_PLATFORM_X11_H */
