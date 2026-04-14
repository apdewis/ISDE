/*
 * panel.h — isde-panel internal header
 */
#ifndef ISDE_PANEL_H
#define ISDE_PANEL_H

#include <ISW/Intrinsic.h>
#include <ISW/IntrinsicP.h>
#include <ISW/StringDefs.h>
#include <ISW/Shell.h>
#include <ISW/Box.h>
#include <ISW/Form.h>
#include <ISW/Label.h>
#include <ISW/List.h>
#include <ISW/Command.h>
#include <ISW/SimpleMenu.h>
#include <ISW/SmeBSB.h>
#include <ISW/SmeLine.h>
#include <ISW/MenuButton.h>
#include <ISW/Viewport.h>
#include <ISW/ISWSVG.h>
#include <ISW/ISWRender.h>

#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>
#include <xcb/randr.h>

#include "isde/isde-ewmh.h"
#include "isde/isde-ipc.h"
#include "isde/isde-dbus.h"
#include "isde/isde-theme.h"
#include "isde/isde-desktop.h"
#include "isde/isde-xdg.h"
#include "isde/isde-config.h"

/* ---------- Panel geometry (scaled) ---------- */
#define PANEL_HEIGHT      40
#define PANEL_ICON_SIZE   22
#define PANEL_BUTTON_PAD  4
#define PANEL_CLOCK_WIDTH 120

/* ---------- Taskbar: one entry per WM_CLASS group ---------- */
typedef struct TaskGroup {
    char       *wm_class;          /* WM_CLASS class name (grouping key) */
    char       *display_name;      /* Display label */
    char       *desktop_exec;      /* Exec from matching .desktop (for launch) */
    char       *desktop_icon;      /* Icon name from matching .desktop */
    char       *icon_path;         /* Resolved SVG path (or NULL) */
    int         desktop_index;     /* Index into Panel.desktop_entries, or -1 */

    xcb_window_t *windows;         /* Array of managed windows in this group */
    int           nwindows;
    int           cap_windows;

    int           pinned;          /* 1 if pinned (stays visible with 0 windows) */

    Widget        button;          /* Command widget on the panel */
    Widget        menu;            /* Window-list popup (OverrideShell + List) */
    Widget        menu_list;       /* List widget inside menu */
    String       *menu_titles;     /* Title array backing menu_list */
    Widget        ctx_menu;        /* Right-click context menu (SimpleMenu) */
    Widget        ctx_close_all;   /* "Close all windows" entry */
    Widget        ctx_close_sep;   /* Separator above pin toggle */
    Widget        ctx_pin;         /* "Pin/Unpin" entry */

    struct TaskGroup *next;
} TaskGroup;

/* ---------- Start menu category ---------- */
/* ---------- Start menu: app entry for a category ---------- */
typedef struct StartMenuApp {
    const char *name;
    const char *icon;
    IsdeDesktopEntry *entry;  /* for isde_desktop_build_exec() at launch */
} StartMenuApp;

typedef struct StartMenuCategory {
    const char    *label;
    StartMenuApp  *apps;
    int            napps;
    int            cap;
} StartMenuCategory;

/* ---------- Panel state ---------- */
typedef struct Panel {
    IswAppContext       app;
    Widget             toplevel;
    Widget             shell;       /* OverrideShell — the panel bar */
    Widget             form;        /* Form layout container */
    Widget             box;         /* Horizontal Box for taskbar buttons */

    /* Applets */
    Widget             start_btn;   /* Start menu button (triangle icon) */
    Widget             start_shell; /* Start menu OverrideShell */
    Widget             cat_viewport;/* Left pane: scrollable viewport */
    Widget             cat_box;     /* Left pane: category buttons */
    Widget             app_viewport;/* Right pane: scrollable viewport */
    Widget             app_box;     /* Right pane: app entries for selected category */
    Widget             menu_toolbar;/* Bottom toolbar strip */
    Widget             shutdown_btn;/* Shut Down button in toolbar */
    Widget             reboot_btn;  /* Reboot button in toolbar */
    Widget             logout_btn;  /* Logout button in toolbar */
    Widget             clock_time;  /* Clock — time label */
    Widget             clock_date;  /* Clock — date label */
    char              *clock_time_fmt; /* strftime format for time */
    char              *clock_date_fmt; /* strftime format for date */

    /* Start menu data */
    StartMenuCategory *categories;
    int                ncategories;
    int                active_cat;  /* Currently displayed category index */
    int                cat_highlight; /* Keyboard-highlighted category */
    int                app_highlight; /* Keyboard-highlighted app */
    int                menu_focus;    /* 0 = category pane, 1 = app pane */

    /* Taskbar */
    TaskGroup         *groups;

    /* Pinned apps (loaded from config) */
    char             **pinned_classes;
    int                npinned;

    /* Desktop entries cache */
    IsdeDesktopEntry **desktop_entries;
    int                ndesktop;

    /* XCB / EWMH */
    xcb_connection_t  *conn;
    xcb_screen_t      *screen;
    xcb_window_t       root;
    int                screen_num;
    IsdeEwmh          *ewmh;
    IsdeIpc           *ipc;

    /* Primary monitor geometry (physical pixels) */
    int16_t            mon_x;
    int16_t            mon_y;
    uint16_t           mon_w;
    uint16_t           mon_h;

    /* HiDPI: physical panel height (PANEL_HEIGHT * scale factor) */
    int                phys_panel_h;

    /* Atoms */
    xcb_atom_t         atom_net_wm_name;
    xcb_atom_t         atom_wm_name;

    /* System tray */
    Widget             tray_box;       /* Box widget for tray icons */
    xcb_window_t      *tray_icons;     /* Embedded icon windows */
    int                ntray;
    int                cap_tray;
    xcb_atom_t         atom_tray_sel;  /* _NET_SYSTEM_TRAY_S<n> */
    xcb_atom_t         atom_tray_opcode; /* _NET_SYSTEM_TRAY_OPCODE */
    xcb_atom_t         atom_xembed;
    xcb_atom_t         atom_xembed_info;

    IswIntervalId       clock_timer;

    /* D-Bus */
    IsdeDBus          *dbus;

    /* Active popup tracking — for click-outside-to-dismiss */
    Widget             active_popup;  /* Currently open popup shell, or NULL */

    int                running;
    int                restart;
} Panel;

/* ---------- panel.c ---------- */
int   panel_init(Panel *p, int *argc, char **argv);
void  panel_show_popup(Panel *p, Widget popup);
void  panel_dismiss_popup(Panel *p);
void  panel_run(Panel *p);
void  panel_cleanup(Panel *p);

/* ---------- taskbar.c ---------- */
void  taskbar_init(Panel *p);
void  taskbar_update(Panel *p);
void  taskbar_highlight_active(Panel *p);
void  taskbar_cleanup(Panel *p);

TaskGroup *taskbar_find_group(Panel *p, const char *wm_class);
TaskGroup *taskbar_add_group(Panel *p, const char *wm_class);

/* ---------- startmenu.c ---------- */
void  startmenu_init(Panel *p);
void  startmenu_cleanup(Panel *p);

/* ---------- tray.c ---------- */
void  tray_init_widgets(Panel *p);  /* create tray box, intern atoms */
void  tray_init_selection(Panel *p); /* claim selection (after realize) */
void  tray_cleanup(Panel *p);

/* ---------- clock.c ---------- */
void  clock_init(Panel *p);
void  clock_cleanup(Panel *p);

#endif /* ISDE_PANEL_H */
