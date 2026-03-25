/*
 * fm.h — isde-fm internal header
 */
#ifndef ISDE_FM_H
#define ISDE_FM_H

#include <X11/Intrinsic.h>
#include <X11/IntrinsicP.h>
#include <X11/StringDefs.h>
#include <X11/Shell.h>
#include <ISW/MainWindow.h>
#include <ISW/MenuBar.h>
#include <ISW/MenuButton.h>
#include <ISW/SimpleMenu.h>
#include <ISW/SmeBSB.h>
#include <ISW/SmeLine.h>
#include <ISW/Command.h>
#include <ISW/Label.h>
#include <ISW/Form.h>
#include <ISW/Paned.h>
#include <ISW/Box.h>
#include <ISW/Viewport.h>
#include <ISW/IconView.h>
#include <ISW/List.h>
#include <ISW/StatusBar.h>
#include <ISW/ISWSVG.h>
#include <ISW/ISWXdnd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#include "isde/isde-config.h"
#include "isde/isde-xdg.h"
#include "isde/isde-desktop.h"

/* ---------- Constants ---------- */
#define FM_ICON_SIZE     48
#define FM_HISTORY_MAX   64

/* ---------- File entry ---------- */
typedef struct {
    char       *name;
    char       *full_path;
    mode_t      mode;
    off_t       size;
    time_t      mtime;
    int         is_dir;
    int         is_hidden;
    const char *mime_icon;  /* icon name from icons.c */
} FmEntry;

/* ---------- View mode ---------- */
typedef enum {
    FM_VIEW_ICON,
    FM_VIEW_LIST
} FmViewMode;

/* ---------- File manager state ---------- */
typedef struct Fm {
    XtAppContext   app;
    Widget         toplevel;
    Widget         main_window;

    /* Navigation bar */
    Widget         nav_box;
    Widget         back_btn;
    Widget         fwd_btn;
    Widget         up_btn;
    Widget         path_label;

    /* Main content */
    Widget         paned;
    Widget         viewport;
    Widget         iconview;    /* current if FM_VIEW_ICON */
    Widget         listview;    /* current if FM_VIEW_LIST */
    FmViewMode     view_mode;
    int            show_hidden;
    int            double_click;  /* 1 = double click to open, 0 = single */

    /* Status bar */
    Widget         status_label;

    /* Current directory */
    char          *cwd;
    FmEntry       *entries;
    int            nentries;

    /* History */
    char          *history[FM_HISTORY_MAX];
    int            hist_pos;
    int            hist_count;

    int            running;
} Fm;

/* ---------- fm.c ---------- */
int   fm_init(Fm *fm, int *argc, char **argv);
void  fm_run(Fm *fm);
void  fm_cleanup(Fm *fm);
void  fm_navigate(Fm *fm, const char *path);
void  fm_refresh(Fm *fm);

/* ---------- browser.c ---------- */
int   browser_read_dir(Fm *fm, const char *path);
void  browser_free_entries(Fm *fm);
void  browser_open_entry(Fm *fm, int index);

/* ---------- fileview.c ---------- */
void  fileview_init(Fm *fm);
void  fileview_populate(Fm *fm);
void  fileview_set_mode(Fm *fm, FmViewMode mode);
void  fileview_cleanup(Fm *fm);

/* ---------- navbar.c ---------- */
void  navbar_init(Fm *fm);
void  navbar_update(Fm *fm);

/* ---------- icons.c ---------- */
void        icons_init(void);
const char *icons_for_entry(const FmEntry *e);
void        icons_cleanup(void);

/* ---------- fileops.c ---------- */
int   fileops_mkdir(Fm *fm, const char *name);
int   fileops_delete(Fm *fm, const char *path);
int   fileops_rename(Fm *fm, const char *oldpath, const char *newname);

#endif /* ISDE_FM_H */
