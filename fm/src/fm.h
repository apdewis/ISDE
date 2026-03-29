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
#include <ISW/Command.h>
#include <ISW/Label.h>
#include <ISW/Form.h>
#include <ISW/Paned.h>
#include <ISW/Box.h>
#include <ISW/FlexBox.h>
#include <ISW/Viewport.h>
#include <ISW/IconView.h>
#include <ISW/List.h>
#include <ISW/StatusBar.h>
#include <ISW/Dialog.h>
#include <ISW/ISWSVG.h>
#include <ISW/ISWXdnd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#include "isde/isde-config.h"
#include "isde/isde-dbus.h"
#include "isde/isde-theme.h"
#include "isde/isde-xdg.h"
#include "isde/isde-desktop.h"
#include "isde/isde-mime.h"

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

/* ---------- Clipboard ---------- */
typedef enum {
    FM_CLIP_NONE,
    FM_CLIP_COPY,
    FM_CLIP_CUT
} FmClipOp;

typedef struct {
    FmClipOp  op;
    char    **paths;
    int       npaths;
    char     *uri_data;    /* cached text/uri-list string */
    char     *gnome_data;  /* cached x-special/gnome-copied-files string */
} FmClipboard;

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

    /* Places sidebar */
    Widget         places_vp;
    Widget         places_box;

    /* Main content */
    Widget         vbox;         /* outer FlexBox (vertical) */
    Widget         hbox;         /* inner FlexBox (horizontal) */
    Widget         viewport;
    Widget         iconview;
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

    /* Desktop entry cache (for "Open with") */
    IsdeDesktopEntry **desktop_entries;
    int                ndesktop;

    /* D-Bus for settings notifications */
    IsdeDBus      *dbus;

    /* Clipboard */
    FmClipboard    clipboard;
    xcb_atom_t     atom_clipboard;
    xcb_atom_t     atom_targets;
    xcb_atom_t     atom_uri_list;
    xcb_atom_t     atom_gnome_files;
    xcb_atom_t     atom_utf8_string;

    int            running;
} Fm;

/* ---------- fm.c ---------- */
int   fm_init(Fm *fm, int *argc, char **argv);
void  fm_run(Fm *fm);
void  fm_cleanup(Fm *fm);
void  fm_navigate(Fm *fm, const char *path);
void  fm_refresh(Fm *fm);
void  fm_register_context_menu(Fm *fm, Widget w);
void  fm_install_shortcuts(Widget w);
void  fm_dismiss_context(void);
void  show_rename_dialog(Fm *fm);

/* ---------- browser.c ---------- */
int   browser_read_dir(Fm *fm, const char *path);
void  browser_free_entries(Fm *fm);
void  browser_open_entry(Fm *fm, int index);

/* ---------- fileview.c ---------- */
void  fileview_init(Fm *fm);
void  fileview_populate(Fm *fm);
void  fileview_cleanup(Fm *fm);

/* ---------- navbar.c ---------- */
void  navbar_init(Fm *fm);
void  navbar_update(Fm *fm);

/* ---------- places.c ---------- */
void  places_init(Fm *fm);
void  places_cleanup(Fm *fm);

/* ---------- icons.c ---------- */
void        icons_init(void);
const char *icons_for_entry(const FmEntry *e);
void        icons_cleanup(void);

/* ---------- fileops.c ---------- */
int   fileops_copy(const char *src, const char *dst);
int   fileops_mkdir(Fm *fm, const char *name);
int   fileops_delete(Fm *fm, const char *path);
int   fileops_rename(Fm *fm, const char *oldpath, const char *newname);
int   fileops_paste(Fm *fm);
int   fileops_trash(const char *path);
int   fileops_restore(const char *trash_name);
int   fileops_empty_trash(void);
char *fileops_trash_path(void);

/* ---------- clipboard.c ---------- */
void  clipboard_init(Fm *fm);
void  clipboard_copy(Fm *fm);
void  clipboard_cut(Fm *fm);
void  clipboard_paste(Fm *fm);
void  clipboard_cleanup(Fm *fm);

/* ---------- dnd.c ---------- */
void  dnd_init(Fm *fm);
void  dnd_cleanup(Fm *fm);

#endif /* ISDE_FM_H */
