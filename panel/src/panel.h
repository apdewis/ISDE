/*
 * panel.h — isde-panel internal header
 */
#ifndef ISDE_PANEL_H
#define ISDE_PANEL_H

#include <X11/Intrinsic.h>
#include <X11/IntrinsicP.h>
#include <X11/StringDefs.h>
#include <X11/Shell.h>
#include <ISW/Box.h>
#include <ISW/Form.h>
#include <ISW/Label.h>
#include <ISW/List.h>
#include <ISW/Command.h>
#include <ISW/SimpleMenu.h>
#include <ISW/SmeBSB.h>
#include <ISW/SmeLine.h>
#include <ISW/MenuButton.h>
#include <ISW/ISWSVG.h>

#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>

#include "isde/isde-ewmh.h"
#include "isde/isde-ipc.h"
#include "isde/isde-desktop.h"
#include "isde/isde-xdg.h"
#include "isde/isde-config.h"

/* ---------- Panel geometry ---------- */
#define PANEL_HEIGHT      32
#define PANEL_ICON_SIZE   22
#define PANEL_BUTTON_PAD   4

/* ---------- Taskbar: one entry per WM_CLASS group ---------- */
typedef struct TaskGroup {
    char       *wm_class;          /* WM_CLASS class name (grouping key) */
    char       *display_name;      /* Display label */
    char       *desktop_exec;      /* Exec from matching .desktop (for launch) */
    char       *desktop_icon;      /* Icon from matching .desktop */

    xcb_window_t *windows;         /* Array of managed windows in this group */
    int           nwindows;
    int           cap_windows;

    int           pinned;          /* 1 if pinned (stays visible with 0 windows) */

    Widget        button;          /* Command widget on the panel */
    Widget        menu;            /* SimpleMenu popup (if >1 window) */

    struct TaskGroup *next;
} TaskGroup;

/* ---------- Start menu category ---------- */
/* ---------- Start menu: app entry for a category ---------- */
typedef struct StartMenuApp {
    const char *name;
    const char *exec;
    const char *icon;
} StartMenuApp;

typedef struct StartMenuCategory {
    const char    *label;
    StartMenuApp  *apps;
    int            napps;
    int            cap;
} StartMenuCategory;

/* ---------- Panel state ---------- */
typedef struct Panel {
    XtAppContext       app;
    Widget             toplevel;
    Widget             shell;       /* OverrideShell — the panel bar */
    Widget             box;         /* Horizontal Box layout */

    /* Applets */
    Widget             start_btn;   /* Start menu button (triangle icon) */
    Widget             start_shell; /* Start menu OverrideShell */
    Widget             cat_box;     /* Left pane: category buttons */
    Widget             app_box;     /* Right pane: app entries for selected category */
    Widget             clock_label; /* Clock */

    /* Start menu data */
    StartMenuCategory *categories;
    int                ncategories;
    int                active_cat;  /* Currently displayed category index */

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

    /* Atoms */
    xcb_atom_t         atom_net_wm_name;
    xcb_atom_t         atom_wm_name;

    XtIntervalId       clock_timer;
    int                running;
} Panel;

/* ---------- panel.c ---------- */
int   panel_init(Panel *p, int *argc, char **argv);
void  panel_run(Panel *p);
void  panel_cleanup(Panel *p);

/* ---------- taskbar.c ---------- */
void  taskbar_init(Panel *p);
void  taskbar_update(Panel *p);
void  taskbar_cleanup(Panel *p);

TaskGroup *taskbar_find_group(Panel *p, const char *wm_class);
TaskGroup *taskbar_add_group(Panel *p, const char *wm_class);

/* ---------- startmenu.c ---------- */
void  startmenu_init(Panel *p);
void  startmenu_cleanup(Panel *p);

/* ---------- clock.c ---------- */
void  clock_init(Panel *p);
void  clock_cleanup(Panel *p);

#endif /* ISDE_PANEL_H */
