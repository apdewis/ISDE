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
#include <ISW/ISWContext.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

#include "isde/isde-config.h"
#include "isde/isde-dbus.h"
#include "isde/isde-theme.h"
#include "isde/isde-xdg.h"
#include "isde/isde-desktop.h"
#include "isde/isde-mime.h"

/* ---------- Constants ---------- */
#define FM_ICON_SIZE     48
#define FM_HISTORY_MAX   64
#define MAX_OPEN_WITH    16

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

/* ---------- Context menu action function ---------- */
struct Fm;
typedef void (*CtxAction)(struct Fm *);

/* ---------- Places sidebar data (opaque, defined in places.c) ---------- */
typedef struct FmPlacesData FmPlacesData;

/* ---------- App-wide shared state ---------- */
typedef struct FmApp {
    XtAppContext   app;
    Widget         first_toplevel;  /* from XtAppInitialize */

    /* Desktop entry cache (shared across windows) */
    IsdeDesktopEntry **desktop_entries;
    int                ndesktop;

    /* D-Bus for settings notifications */
    IsdeDBus      *dbus;

    /* Icon cache (moved from icons.c statics) */
    char *icon_folder;
    char *icon_file;
    char *icon_exec;
    char *icon_image;
    char *icon_theme;

    /* Clipboard atoms (per-display, shared) */
    xcb_atom_t     atom_clipboard;
    xcb_atom_t     atom_targets;
    xcb_atom_t     atom_uri_list;
    xcb_atom_t     atom_gnome_files;
    xcb_atom_t     atom_utf8_string;

    /* Window tracking */
    struct Fm    **windows;
    int            nwindows;
    struct Fm     *clipboard_owner;  /* which window owns CLIPBOARD */

    char          *initial_path;  /* from argv, used by fm_app_init */
    int            running;
} FmApp;

/* ---------- Per-window file manager state ---------- */
typedef struct Fm {
    FmApp         *app_state;
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
    FmPlacesData  *places_data;

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

    /* Clipboard */
    FmClipboard    clipboard;

    /* Rename dialog (per-window) */
    Widget         rename_shell;
    int            rename_index;

    /* Delete confirmation dialog (per-window) */
    Widget         delete_shell;

    /* Empty trash dialog (per-window) */
    Widget         empty_trash_shell;

    /* Context menu (per-window) */
    Widget         ctx_shell;
    Widget         ctx_list;
    int            ctx_in_trash;
    String        *dyn_labels;
    CtxAction     *dyn_actions;
    int            dyn_nitems;

    /* "Open with" state (per-window) */
    int            ow_indices[MAX_OPEN_WITH];
    int            ow_count;
    char          *ow_label_buf[MAX_OPEN_WITH];
    char          *ow_file_path;

    /* Fileview backing arrays (per-window) */
    String        *fv_labels;
    String        *fv_icons;
    char         **fv_trunc_names;
    int            fv_trunc_count;

    /* Double-click tracking (per-window) */
    int            last_click_index;
    struct timespec last_click_time;

    /* DnD state (per-window) */
    xcb_button_press_event_t dnd_saved_press;
    Boolean        dnd_press_valid;
    char         **dnd_drag_paths;
    int            dnd_ndrag_paths;
    Boolean        dnd_drop_was_noop;
} Fm;

/* ---------- Context for storing Fm* on shell windows ---------- */
extern XContext fm_window_context;  /* initialized once in fm_init */

/* Store Fm* on a shell widget's window */
static inline void fm_set_context(Widget shell, Fm *fm)
{
    IswSaveContext(XtDisplay(shell), XtWindow(shell),
                   fm_window_context, (void *)fm);
}

/* Recover Fm* from any widget by walking up through shells ---------- */
static inline Fm *fm_from_widget(Widget w)
{
    /* Walk up through the widget tree, checking each shell for
     * a stored Fm context.  Transient/override shells (dialogs,
     * context menus) won't have one, but their parent toplevel will. */
    while (w) {
        if (XtIsShell(w) && XtIsRealized(w)) {
            void *data = NULL;
            if (IswFindContext(XtDisplay(w), XtWindow(w),
                               fm_window_context, &data) == 0 && data)
                return (Fm *)data;
        }
        w = XtParent(w);
    }
    return NULL;
}

/* ---------- fm.c ---------- */
int    fm_app_init(FmApp *app, int *argc, char **argv);
void   fm_app_run(FmApp *app);
void   fm_app_cleanup(FmApp *app);
Fm    *fm_window_new(FmApp *app, const char *path);
void   fm_window_destroy(Fm *fm);
void   fm_navigate(Fm *fm, const char *path);
void   fm_refresh(Fm *fm);
void   fm_register_context_menu(Fm *fm, Widget w);
void   fm_install_shortcuts(Widget w);
void   fm_dismiss_context(Fm *fm);
void   show_rename_dialog(Fm *fm);

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
void        icons_init(FmApp *app);
const char *icons_for_entry(FmApp *app, const FmEntry *e);
void        icons_cleanup(FmApp *app);

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

/* ---------- instance.c ---------- */
int   instance_try_primary(FmApp *app, const char *path);

#endif /* ISDE_FM_H */
