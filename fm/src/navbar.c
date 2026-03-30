#define _POSIX_C_SOURCE 200809L
/*
 * navbar.c — navigation bar: back, forward, up, path display
 */
#include "fm.h"

#include <stdlib.h>
#include <string.h>

/* Navigate to a history entry without pushing new history */
static void navigate_history(Fm *fm, int pos)
{
    if (pos < 0 || pos >= fm->hist_count) { return; }
    fm->hist_pos = pos;
    free(fm->cwd);
    fm->cwd = strdup(fm->history[pos]);
    fm_refresh(fm);
}

static void back_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
    if (fm->hist_pos > 0) {
        navigate_history(fm, fm->hist_pos - 1);
    }
}

static void fwd_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
    if (fm->hist_pos < fm->hist_count - 1) {
        navigate_history(fm, fm->hist_pos + 1);
    }
}

static void up_cb(Widget w, XtPointer cd, XtPointer call)
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
                              XtCallbackProc cb)
{
    Arg args[20];
    Cardinal n = 0;

    char *icon_path = isde_icon_find("actions", icon_name);
    if (icon_path) {
        XtSetArg(args[n], XtNsvgFile, icon_path);  n++;
        XtSetArg(args[n], XtNlabel, "");            n++;
    } else {
        XtSetArg(args[n], XtNlabel, fallback_label); n++;
    }
    XtSetArg(args[n], XtNwidth, isde_scale(32));     n++;
    XtSetArg(args[n], XtNheight, isde_scale(32));  n++;
    XtSetArg(args[n], XtNinternalWidth, 0);        n++;
    XtSetArg(args[n], XtNinternalHeight, 0);       n++;
    XtSetArg(args[n], XtNborderWidth, 0);          n++;

    Widget btn = XtCreateManagedWidget(name, commandWidgetClass,
                                       fm->nav_box, args, n);
    XtAddCallback(btn, XtNcallback, cb, fm);

    /* icon_path intentionally not freed — XtNsvgFile may hold the pointer */
    return btn;
}

void navbar_init(Fm *fm)
{
    Arg args[20];
    Cardinal n;

    n = 0;
    XtSetArg(args[n], XtNorientation, XtorientHorizontal); n++;
    XtSetArg(args[n], XtNborderWidth, 1);                   n++;
    XtSetArg(args[n], XtNhSpace, isde_scale(4));              n++;
    XtSetArg(args[n], XtNvSpace, isde_scale(2));             n++;
    XtSetArg(args[n], XtNheight, isde_scale(36));            n++;
    fm->nav_box = XtCreateManagedWidget("navBar", boxWidgetClass,
                                        fm->vbox, args, n);

    fm->back_btn = make_nav_button(fm, "backBtn", "go-back", "<", back_cb);
    fm->fwd_btn  = make_nav_button(fm, "fwdBtn", "go-forward", ">", fwd_cb);
    fm->up_btn   = make_nav_button(fm, "upBtn", "go-up", "Up", up_cb);

    n = 0;
    XtSetArg(args[n], XtNlabel, "/");              n++;
    XtSetArg(args[n], XtNborderWidth, 0);           n++;
    fm->path_label = XtCreateManagedWidget("pathLabel", labelWidgetClass,
                                           fm->nav_box, args, n);
}

void navbar_update(Fm *fm)
{
    Arg args[20];

    XtSetArg(args[0], XtNlabel, fm->cwd ? fm->cwd : "/");
    XtSetValues(fm->path_label, args, 1);

    XtSetArg(args[0], XtNsensitive, fm->hist_pos > 0);
    XtSetValues(fm->back_btn, args, 1);

    XtSetArg(args[0], XtNsensitive, fm->hist_pos < fm->hist_count - 1);
    XtSetValues(fm->fwd_btn, args, 1);

    XtSetArg(args[0], XtNsensitive,
             fm->cwd && strcmp(fm->cwd, "/") != 0);
    XtSetValues(fm->up_btn, args, 1);
}
