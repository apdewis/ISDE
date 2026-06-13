/*
 * wm.h — isde-wm internal header
 */
#ifndef ISDE_WM_H
#define ISDE_WM_H

#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xcb.h>

#include "ewmh.h"
#include "dbus.h"
#include "isde-theme.h"
#include "isde-xdg.h"

#include "render.h"

/* ---------- Frame geometry (physical pixels) ---------- */
#define WM_TITLE_HEIGHT    isde_font_height("title", 10)
#define WM_BORDER_WIDTH     0
#define WM_BUTTON_SIZE     16

#include "randr.h"

#ifdef ISDE_COMPOSITOR
#include "compositor.h"
#endif

typedef IsdeMonitor MonitorGeom;

/* ---------- Timer ---------- */
#define WM_MAX_TIMERS 16

typedef void (*WmTimerCallback)(void *data);

typedef struct WmTimer {
    uint64_t          deadline_ms;
    WmTimerCallback   callback;
    void             *data;
    int               active;
} WmTimer;

/* ---------- Startup notification sequence ---------- */
#define STARTUP_TIMEOUT_MS 30000

typedef struct WmStartupSeq {
    char        *id;
    char        *wmclass;
    uint32_t     timestamp;
    int          timer_id;
    struct WmStartupSeq *next;
} WmStartupSeq;

/* ---------- Title bar button hit zones ---------- */
#define FRAME_BTN_MENU     0
#define FRAME_BTN_MINIMIZE 1
#define FRAME_BTN_MAXIMIZE 2
#define FRAME_BTN_CLOSE    3

/* ---------- Client (managed window) ---------- */
typedef struct WmClient {
    xcb_window_t client;       /* The application window */

    /* Frame window (override-redirect, parent of client).
     * Title-bar buttons are hit-tested by coordinate on this window —
     * there are no separate button sub-windows. */
    xcb_window_t frame;

    /* Cairo surface/context for frame rendering (NULL if undecorated) */
    cairo_surface_t *frame_surface;

    int16_t      x, y;         /* Frame position (physical pixels) */
    uint16_t     width, height;/* Client area size (excludes frame) */
    int          focused;
    int          maximized;
    int          fullscreen;   /* _NET_WM_STATE_FULLSCREEN */
    int          minimized;
    int          above;        /* _NET_WM_STATE_ABOVE */
    int          below;        /* _NET_WM_STATE_BELOW */
    int          modal;        /* _NET_WM_STATE_MODAL */
    int          sticky;       /* _NET_WM_STATE_STICKY */
    int          skip_taskbar; /* _NET_WM_STATE_SKIP_TASKBAR */
    int          skip_pager;   /* _NET_WM_STATE_SKIP_PAGER */
    int          demands_attention; /* _NET_WM_STATE_DEMANDS_ATTENTION */
    int          hidden;       /* WM-initiated unmap (desktop switch) */
    int          ignore_unmap; /* count of pending self-induced client unmaps */
    int          decorated;     /* 0 = CSD/no frame chrome */
    int          mapped;       /* frame is mapped */
    uint32_t     desktop;      /* _NET_WM_DESKTOP (0xFFFFFFFF = sticky) */
    xcb_window_t transient_for; /* WM_TRANSIENT_FOR parent (0 = none) */
    /* Saved geometry for restore from maximize */
    int16_t      save_x, save_y;
    uint16_t     save_w, save_h;
    char        *title;
    char        *icon_name;
    char        *visible_name;
    char        *visible_icon_name;
    unsigned long focus_seq;       /* focus sequence number for MRU fallback */
    uint32_t     user_time;        /* _NET_WM_USER_TIME (0 = don't focus) */
    xcb_window_t user_time_window; /* _NET_WM_USER_TIME_WINDOW (0 = none) */
    char        *startup_id;      /* _NET_STARTUP_ID (NULL = none) */

    /* ICCCM size hints (WM_NORMAL_HINTS) */
    int          min_w, min_h;
    int          max_w, max_h;
    int          base_w, base_h;
    int          inc_w, inc_h;
    int          fixed_size;       /* 1 if min == max in both dimensions */

    /* Resize grips — input-only windows on frame edges */
    xcb_window_t  grip[8];  /* top, bottom, left, right, tl, tr, bl, br */

    struct WmClient *next;
} WmClient;

/* ---------- WM state ---------- */
typedef struct Wm {
    /* XCB connection */
    xcb_connection_t      *conn;
    xcb_screen_t          *screen;
    xcb_window_t           root;
    int                    screen_num;

    /* EWMH / ICCCM */
    IsdeEwmh              *ewmh;

    /* IPC — disabled pending DBus replacement; field retained for the
       commented call sites in wm.c/keys.c. */
    //IsdeIpc               *ipc;
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
    xcb_atom_t             atom_wm_icon_name;
    xcb_atom_t             atom_net_wm_icon_name;
    xcb_atom_t             atom_net_wm_user_time;
    xcb_atom_t             atom_net_wm_user_time_window;
    xcb_atom_t             atom_net_wm_state_focused;
    xcb_atom_t             atom_net_startup_info_begin;
    xcb_atom_t             atom_net_startup_info;
    xcb_atom_t             atom_net_startup_id;

    /* Startup notification */
    WmStartupSeq          *startup_seqs;
    char                   sn_buf[2048];
    int                    sn_buf_len;

    /* Client list */
    WmClient              *clients;
    WmClient              *focused;
    unsigned long          focus_seq;  /* monotonic counter for MRU tracking */
    xcb_timestamp_t        last_user_time; /* most recent user interaction */

    /* Unmanaged dock windows (_NET_WM_WINDOW_TYPE_DOCK) */
    xcb_window_t          *docks;
    int                    ndocks;
    int                    cap_docks;

    /* Virtual desktops */
    int                    desk_rows;
    int                    desk_cols;
    int                    num_desktops;
    uint32_t               current_desktop;
    xcb_window_t           desk_osd;       /* OSD popup window */
    int                    desk_osd_timer;  /* timer ID (-1 = none) */
    int                    desk_reload_tries; /* EWMH-layout poll attempts */

    /* Window switcher (Alt+Tab) */
    xcb_window_t           switcher_shell;   /* OSD popup window */
    WmClient             **switcher_order;   /* MRU-sorted client array */
    char                 **switcher_labels;  /* title strings for list */
    int                    switcher_count;   /* number of entries */
    int                    switcher_visible; /* number of visible label rows */
    int                    switcher_sel;     /* currently highlighted index */
    int                    switcher_active;  /* 1 while Alt is held */

    /* Active window menu */
    xcb_window_t           win_menu;        /* popup window (0 = none) */
    WmClient              *menu_client;
    int                    menu_item_count;
    int                    menu_item_height;

    /* Drag state */
    enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE } drag_mode;
    int                    resize_edge;
    WmClient              *drag_client;
    int16_t                drag_start_x, drag_start_y;
    int16_t                drag_orig_x, drag_orig_y;
    uint16_t               drag_orig_w, drag_orig_h;

    /* Title bar button press feedback */
    WmClient              *btn_press_client; /* client whose button is held */
    int                    btn_press_btn;    /* FRAME_BTN_* or -1 */
    int                    btn_press_hover;  /* pointer currently over it */

    /* Snap preview */
    xcb_window_t           snap_preview;   /* overlay window (0 = none) */
    int                    snap_pending;   /* pending snap zone */
    int                    snap_monitor;   /* monitor index for pending snap */

    /* Per-monitor geometry (physical pixels) */
    MonitorGeom           *monitors;
    int                    nmonitors;
    uint8_t                randr_event_base;

    /* Resize cursors */
    xcb_cursor_t           cursors[8];

    /* Title bar height (physical pixels, already scaled for HiDPI) */
    int                    title_height;

    /* HiDPI scale factor (Xft.dpi / 96, or ISW_SCALE_FACTOR; >= 1.0).
     * Decoration metrics are design pixels multiplied by this. */
    double                 scale_factor;

    /* WM_Sn selection (ICCCM WM replacement protocol) */
    xcb_atom_t             atom_wm_sn;
    xcb_window_t           wm_sn_owner;
    int                    replace;

    /* Timer system */
    WmTimer                timers[WM_MAX_TIMERS];

    /* Cached SVG icon surfaces for title bar buttons.
     * *_inv variants are tinted with the button background colour, for the
     * inverted (pressed) state where bg and fg are swapped. */
    cairo_surface_t       *icon_minimize;
    cairo_surface_t       *icon_maximize;
    cairo_surface_t       *icon_restore;
    cairo_surface_t       *icon_close;
    cairo_surface_t       *icon_menu;
    cairo_surface_t       *icon_minimize_inv;
    cairo_surface_t       *icon_maximize_inv;
    cairo_surface_t       *icon_restore_inv;
    cairo_surface_t       *icon_close_inv;
    cairo_surface_t       *icon_menu_inv;

    int                    running;
    int                    restart;

#ifdef ISDE_COMPOSITOR
    WmCompositor          *compositor;
#endif
} Wm;

/* Scale a design (logical, 96-DPI) pixel value to physical pixels for the
 * current HiDPI scale factor. */
static inline int wm_scale(const Wm *wm, int v)
{
    return (int)(v * wm->scale_factor + 0.5);
}

/* ---------- wm.c — core ---------- */
int   wm_init(Wm *wm, int *argc, char **argv);
void  wm_run(Wm *wm);
void  wm_cleanup(Wm *wm);

WmClient *wm_find_client_by_frame(Wm *wm, xcb_window_t frame);
WmClient *wm_find_client_by_window(Wm *wm, xcb_window_t win);
void      wm_focus_client(Wm *wm, WmClient *c, xcb_timestamp_t time);
void      wm_remove_client(Wm *wm, WmClient *c);
void      wm_close_client(Wm *wm, WmClient *c);
void      wm_maximize_client(Wm *wm, WmClient *c);
void      wm_fullscreen_client(Wm *wm, WmClient *c, int enable);
void      wm_minimize_client(Wm *wm, WmClient *c);
void      wm_restore_client(Wm *wm, WmClient *c);
void      wm_set_above(Wm *wm, WmClient *c, int enable);
void      wm_set_below(Wm *wm, WmClient *c, int enable);
void      wm_move_to_desktop(Wm *wm, WmClient *c, uint32_t desktop);
void      wm_restack_above_below(Wm *wm);

/* ---------- wm.c — work area / monitors ---------- */
void  wm_get_work_area(Wm *wm, int *wx, int *wy, int *ww, int *wh);
void  wm_get_primary_monitor(Wm *wm, int *mx, int *my, int *mw, int *mh);

/* ---------- wm.c — decoration checks ---------- */
int   wm_client_wants_decorations(Wm *wm, xcb_window_t win);
int   wm_window_type_wants_decorations(Wm *wm, xcb_window_t win);

/* ---------- wm.c — timer system ---------- */
int       wm_timer_add(Wm *wm, uint32_t ms, WmTimerCallback cb, void *data);
void      wm_timer_remove(Wm *wm, int id);
uint64_t  wm_now_ms(void);
int       wm_timer_next_timeout(Wm *wm);

/* ---------- placement.c — window placement ---------- */
void  wm_place_client(Wm *wm, WmClient *c);

/* ---------- frame.c — frame decoration ---------- */
WmClient *frame_create(Wm *wm, xcb_window_t client, int adopt);
void      frame_destroy(Wm *wm, WmClient *c);
void      frame_update_title(Wm *wm, WmClient *c);
void      frame_configure(Wm *wm, WmClient *c);
void      frame_set_extents(Wm *wm, WmClient *c);
int       frame_total_width(WmClient *c);
int       frame_total_height(Wm *wm, WmClient *c);
void      frame_apply_theme(Wm *wm, WmClient *c);
void      frame_refresh_theme(Wm *wm, WmClient *c);
void      frame_paint(Wm *wm, WmClient *c);
void      wm_dismiss_menu(Wm *wm);
void      frame_disambiguate_all(Wm *wm, const char *base_title,
                                 const char *base_icon);
void      frame_read_size_hints(Wm *wm, WmClient *c);
void      frame_create_grips(Wm *wm, WmClient *c);
void      frame_update_grips(Wm *wm, WmClient *c);
void      frame_destroy_grips(Wm *wm, WmClient *c);
void      frame_init_cursors(Wm *wm);
void      frame_init_icons(Wm *wm);

/* Identify which title bar button is at frame-relative (x,y).
 * Returns FRAME_BTN_* or -1 if the point is not on a button. */
int       frame_button_at(Wm *wm, WmClient *c, int x, int y);

/* Show/handle window menu */
void      win_menu_show(Wm *wm, WmClient *c);
int       win_menu_click(Wm *wm, int16_t x, int16_t y);

/* Grip indices — match the grip[8] array order */
#define GRIP_TOP    0
#define GRIP_BOTTOM 1
#define GRIP_LEFT   2
#define GRIP_RIGHT  3
#define GRIP_TL     4
#define GRIP_TR     5
#define GRIP_BL     6
#define GRIP_BR     7

/* ---------- wm.c — startup notification ---------- */
WmStartupSeq *wm_find_startup_seq(Wm *wm, const char *id);
WmStartupSeq *wm_find_startup_seq_by_wmclass(Wm *wm, const char *instance,
                                              const char *class_name);
void           wm_remove_startup_seq(Wm *wm, WmStartupSeq *seq);

/* ---------- ewmh.c — EWMH property management ---------- */
void  wm_ewmh_setup(Wm *wm);
void  wm_ewmh_update_client_list(Wm *wm);
void  wm_ewmh_update_client_list_stacking(Wm *wm);
void  wm_ewmh_update_active(Wm *wm);
void  wm_ewmh_update_workarea(Wm *wm);
void  wm_ewmh_set_allowed_actions(Wm *wm, WmClient *c);

/* ---------- desktops.c — virtual desktop management ---------- */
void  wm_desktops_init(Wm *wm);
void  wm_desktops_settings_changed(Wm *wm);
void  wm_desktops_switch(Wm *wm, uint32_t desktop);
void  wm_desktops_move(Wm *wm, int dx, int dy);
void  wm_desktops_show_osd(Wm *wm);

/* ---------- keys.c — key binding handling ---------- */
void  wm_keys_setup(Wm *wm);
void  wm_keys_handle(Wm *wm, xcb_key_press_event_t *ev);
void  wm_keys_handle_release(Wm *wm, xcb_key_release_event_t *ev);

/* ---------- switcher.c — Alt+Tab window switcher OSD ---------- */
void  wm_switcher_show(Wm *wm);
void  wm_switcher_next(Wm *wm);
void  wm_switcher_prev(Wm *wm);
void  wm_switcher_commit(Wm *wm);
void  wm_switcher_cancel(Wm *wm);

#endif /* ISDE_WM_H */
