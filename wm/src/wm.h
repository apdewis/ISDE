/*
 * wm.h — isde-wm internal header
 */
#ifndef ISDE_WM_H
#define ISDE_WM_H

#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/Shell.h>
#include <X11/IntrinsicP.h>
#include <ISW/Label.h>
#include <ISW/Command.h>

#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>

#include "isde/isde-ewmh.h"
#include "isde/isde-ipc.h"
#include "isde/isde-dbus.h"
#include "isde/isde-theme.h"
#include "isde/isde-xdg.h"

/* ---------- Frame geometry ---------- */
/* Logical title height — ISW auto-scales during widget creation.
 * For physical pixel calculations (XtConfigureWidget, XCB ops),
 * use wm->title_height which is set after the first frame creation. */
#define WM_TITLE_HEIGHT    isde_font_height("title", 10)
#define WM_BORDER_WIDTH     0
#define WM_BUTTON_SIZE     16

/* ---------- Client (managed window) ---------- */
typedef struct WmClient {
    xcb_window_t client;       /* The application window */

    /* ISW widget tree for the frame */
    Widget       shell;        /* OverrideShell — the frame window */
    Widget       title_label;  /* Label — window title */
    Widget       minimize_btn; /* Command — minimize (placeholder) */
    Widget       maximize_btn; /* Command — maximize/restore */
    Widget       close_btn;    /* Command — close button */

    int16_t      x, y;         /* Frame position */
    uint16_t     width, height;/* Client area size (excludes frame) */
    int          focused;
    int          maximized;
    int          minimized;
    int          decorated;     /* 0 = CSD/no frame chrome */
    uint32_t     desktop;      /* _NET_WM_DESKTOP (0xFFFFFFFF = sticky) */
    xcb_window_t transient_for; /* WM_TRANSIENT_FOR parent (0 = none) */
    /* Saved geometry for restore from maximize */
    int16_t      save_x, save_y;
    uint16_t     save_w, save_h;
    char        *title;
    unsigned long focus_seq;       /* focus sequence number for MRU fallback */

    /* Resize grips — input-only windows on frame edges */
    xcb_window_t  grip[8];  /* top, bottom, left, right, tl, tr, bl, br */

    struct WmClient *next;
} WmClient;

/* ---------- WM state ---------- */
typedef struct Wm {
    /* Xt */
    XtAppContext           app;
    Widget                 toplevel;

    /* XCB (obtained from Xt's display) */
    xcb_connection_t      *conn;
    xcb_screen_t          *screen;
    xcb_window_t           root;
    int                    screen_num;

    /* EWMH / ICCCM */
    IsdeEwmh              *ewmh;

    /* IPC */
    IsdeIpc               *ipc;
    IsdeDBus              *dbus;

    /* Key bindings */
    xcb_key_symbols_t     *keysyms;

    /* Atoms we intern ourselves */
    xcb_atom_t             atom_wm_protocols;
    xcb_atom_t             atom_wm_delete_window;
    xcb_atom_t             atom_wm_take_focus;
    xcb_atom_t             atom_wm_name;
    xcb_atom_t             atom_net_wm_name;
    xcb_atom_t             atom_motif_wm_hints;
    xcb_atom_t             atom_wm_change_state;

    /* Client list */
    WmClient              *clients;
    WmClient              *focused;
    unsigned long          focus_seq;  /* monotonic counter for MRU tracking */

    /* Virtual desktops */
    int                    desk_rows;
    int                    desk_cols;
    int                    num_desktops;
    uint32_t               current_desktop;
    Widget                 desk_osd;       /* OSD popup shell */
    XtIntervalId           desk_osd_timer; /* auto-hide timer */

    /* Drag state */
    enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE } drag_mode;
    int                    resize_edge;
    WmClient              *drag_client;
    int16_t                drag_start_x, drag_start_y;
    int16_t                drag_orig_x, drag_orig_y;
    uint16_t               drag_orig_w, drag_orig_h;

    /* Snap preview */
    xcb_window_t           snap_preview;   /* overlay window (0 = none) */
    int                    snap_pending;   /* pending snap zone */

    /* Resize cursors */
    xcb_cursor_t           cursors[8];

    /* Physical (HiDPI-scaled) title bar height, set after first frame */
    int                    title_height;

    int                    running;
    int                    restart;
} Wm;

/* ---------- wm.c — core ---------- */
int   wm_init(Wm *wm, int *argc, char **argv);
void  wm_run(Wm *wm);
void  wm_cleanup(Wm *wm);

WmClient *wm_find_client_by_frame(Wm *wm, xcb_window_t frame);
WmClient *wm_find_client_by_widget(Wm *wm, Widget w);
WmClient *wm_find_client_by_window(Wm *wm, xcb_window_t win);
void      wm_focus_client(Wm *wm, WmClient *c);
void      wm_remove_client(Wm *wm, WmClient *c);
void      wm_close_client(Wm *wm, WmClient *c);
void      wm_maximize_client(Wm *wm, WmClient *c);
void      wm_minimize_client(Wm *wm, WmClient *c);

/* ---------- wm.c — work area ---------- */
void  wm_get_work_area(Wm *wm, int *wx, int *wy, int *ww, int *wh);

/* ---------- wm.c — decoration checks ---------- */
int   wm_client_wants_decorations(Wm *wm, xcb_window_t win);
int   wm_window_type_wants_decorations(Wm *wm, xcb_window_t win);

/* ---------- placement.c — window placement ---------- */
void  wm_place_client(Wm *wm, WmClient *c);

/* ---------- frame.c — frame decoration ---------- */
WmClient *frame_create(Wm *wm, xcb_window_t client);
void      frame_destroy(Wm *wm, WmClient *c);
void      frame_update_title(Wm *wm, WmClient *c);
void      frame_configure(Wm *wm, WmClient *c);
int       frame_total_width(WmClient *c);
int       frame_total_height(Wm *wm, WmClient *c);
void      frame_apply_theme(Wm *wm, WmClient *c);
void      frame_create_grips(Wm *wm, WmClient *c);
void      frame_update_grips(Wm *wm, WmClient *c);
void      frame_destroy_grips(Wm *wm, WmClient *c);
void      frame_init_cursors(Wm *wm);

/* Grip indices — match the grip[8] array order */
#define GRIP_TOP    0
#define GRIP_BOTTOM 1
#define GRIP_LEFT   2
#define GRIP_RIGHT  3
#define GRIP_TL     4
#define GRIP_TR     5
#define GRIP_BL     6
#define GRIP_BR     7

/* ---------- ewmh.c — EWMH property management ---------- */
void  wm_ewmh_setup(Wm *wm);
void  wm_ewmh_update_client_list(Wm *wm);
void  wm_ewmh_update_active(Wm *wm);

/* ---------- desktops.c — virtual desktop management ---------- */
void  wm_desktops_init(Wm *wm);
void  wm_desktops_switch(Wm *wm, uint32_t desktop);
void  wm_desktops_move(Wm *wm, int dx, int dy);
void  wm_desktops_show_osd(Wm *wm);

/* ---------- keys.c — key binding handling ---------- */
void  wm_keys_setup(Wm *wm);
void  wm_keys_handle(Wm *wm, xcb_key_press_event_t *ev);

#endif /* ISDE_WM_H */
