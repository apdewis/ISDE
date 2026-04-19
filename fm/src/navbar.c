#define _POSIX_C_SOURCE 200809L
/*
 * navbar.c — navigation bar: back, forward, up, path display
 */
#include "fm.h"

#include <stdlib.h>
#include <string.h>
#include <ISW/IswArgMacros.h>

/* ---------- view mode callbacks ---------- */

static void icon_view_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
    fileview_set_mode(fm, FM_VIEW_ICON);
}

static void list_view_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
    fileview_set_mode(fm, FM_VIEW_LIST);
}

/* Navigate to a history entry without pushing new history */
static void navigate_history(Fm *fm, int pos)
{
    if (pos < 0 || pos >= fm->hist_count) { return; }
    fm->hist_pos = pos;
    free(fm->cwd);
    fm->cwd = strdup(fm->history[pos]);
    fm_refresh(fm);
}

static void back_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
    if (fm->hist_pos > 0) {
        navigate_history(fm, fm->hist_pos - 1);
    }
}

static void fwd_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
    if (fm->hist_pos < fm->hist_count - 1) {
        navigate_history(fm, fm->hist_pos + 1);
    }
}

static void up_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
    char *parent = strdup(fm->cwd);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = '\0';
    } else {
        free(parent);
        parent = strdup("/");
    }
    fm_navigate(fm, parent);
    free(parent);
}

/* Create a nav button with an SVG icon from the theme */
static Widget make_nav_button(Fm *fm, const char *name,
                              const char *icon_name,
                              const char *fallback_label,
                              IswCallbackProc cb)
{
    IswArgBuilder ab = IswArgBuilderInit();

    char *icon_path = isde_icon_find("actions", icon_name);
    if (icon_path) {
        IswArgImage(&ab, icon_path);
        IswArgLabel(&ab, "");
    } else {
        IswArgLabel(&ab, fallback_label);
    }
    IswArgWidth(&ab, 32);
    IswArgHeight(&ab, 32);
    IswArgInternalWidth(&ab, 0);
    IswArgInternalHeight(&ab, 0);

    Widget btn = IswCreateManagedWidget(name, commandWidgetClass,
                                       fm->nav_box, ab.args, ab.count);
    IswAddCallback(btn, IswNcallback, cb, fm);

    /* icon_path intentionally not freed — IswNimage may hold the pointer */
    return btn;
}

void navbar_init(Fm *fm)
{
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgBorderWidth(&ab, 1);
    IswArgHSpace(&ab, 4);
    IswArgVSpace(&ab, 2);
    IswArgHeight(&ab, 36);
    fm->nav_box = IswCreateManagedWidget("navBar", toolbarWidgetClass,
                                        fm->vbox, ab.args, ab.count);

    fm->back_btn = make_nav_button(fm, "backBtn", "go-back", "<", back_cb);
    fm->fwd_btn  = make_nav_button(fm, "fwdBtn", "go-forward", ">", fwd_cb);
    fm->up_btn   = make_nav_button(fm, "upBtn", "go-up", "Up", up_cb);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "/");
    IswArgBorderWidth(&ab, 0);
    fm->path_label = IswCreateManagedWidget("pathLabel", labelWidgetClass,
                                           fm->nav_box, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgToolbarAlignment(&ab, IswToolbarAlignCenter);
    IswSetValues(fm->path_label, ab.args, ab.count);

    /* View mode toggle buttons — right-aligned */
    fm->icon_view_btn = make_nav_button(fm, "iconViewBtn", "view-grid",
                                        "Grid", icon_view_cb);
    fm->list_view_btn = make_nav_button(fm, "listViewBtn", "view-list",
                                        "List", list_view_cb);

    IswArgBuilderReset(&ab);
    IswArgToolbarAlignment(&ab, IswToolbarAlignRight);
    IswSetValues(fm->icon_view_btn, ab.args, ab.count);
    IswSetValues(fm->list_view_btn, ab.args, ab.count);
}

void navbar_update(Fm *fm)
{
    Arg args[20];

    IswSetArg(args[0], IswNlabel, fm->cwd ? fm->cwd : "/");
    IswSetValues(fm->path_label, args, 1);

    IswSetArg(args[0], IswNsensitive, fm->hist_pos > 0);
    IswSetValues(fm->back_btn, args, 1);

    IswSetArg(args[0], IswNsensitive, fm->hist_pos < fm->hist_count - 1);
    IswSetValues(fm->fwd_btn, args, 1);

    IswSetArg(args[0], IswNsensitive,
             fm->cwd && strcmp(fm->cwd, "/") != 0);
    IswSetValues(fm->up_btn, args, 1);
}
