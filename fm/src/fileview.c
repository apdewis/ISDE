#define _POSIX_C_SOURCE 200809L
/*
 * fileview.c — populate IconView or List from browser entries
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- double-click tracking ---------- */

static int    last_click_index = -1;
static struct timespec last_click_time;

static int is_double_click(int index)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long ms = (now.tv_sec - last_click_time.tv_sec) * 1000 +
              (now.tv_nsec - last_click_time.tv_nsec) / 1000000;

    int dbl = (index == last_click_index &&
               ms < isde_config_double_click_ms());

    last_click_index = index;
    last_click_time = now;

    return dbl;
}

/* ---------- IconView callbacks ---------- */

static void iconview_callback(Widget w, XtPointer client_data,
                              XtPointer call_data)
{
    (void)w;
    Fm *fm = (Fm *)client_data;
    IswIconViewCallbackData *d = (IswIconViewCallbackData *)call_data;

    fm_dismiss_context();

    if (fm->double_click) {
        if (is_double_click(d->index))
            browser_open_entry(fm, d->index);
    } else {
        browser_open_entry(fm, d->index);
    }
}

/* ---------- List callback ---------- */

static void listview_callback(Widget w, XtPointer client_data,
                              XtPointer call_data)
{
    (void)w;
    Fm *fm = (Fm *)client_data;
    IswListReturnStruct *ret = (IswListReturnStruct *)call_data;
    if (ret->list_index < 0) return;

    fm_dismiss_context();

    if (fm->double_click) {
        if (is_double_click(ret->list_index))
            browser_open_entry(fm, ret->list_index);
    } else {
        browser_open_entry(fm, ret->list_index);
    }
}

/* ---------- init / populate ---------- */

void fileview_init(Fm *fm)
{
    fm->view_mode = FM_VIEW_ICON;

    /* Viewport for scrolling — below the nav bar */
    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNallowVert, True);        n++;
    XtSetArg(args[n], XtNallowHoriz, True);       n++;
    XtSetArg(args[n], XtNuseRight, True);         n++;
    XtSetArg(args[n], XtNborderWidth, 0);         n++;
    XtSetArg(args[n], XtNfromVert, fm->nav_box);  n++;
    XtSetArg(args[n], XtNtop, XtChainTop);        n++;
    XtSetArg(args[n], XtNbottom, XtChainBottom);  n++;
    XtSetArg(args[n], XtNleft, XtChainLeft);      n++;
    XtSetArg(args[n], XtNright, XtChainRight);    n++;
    fm->viewport = XtCreateManagedWidget("viewport", viewportWidgetClass,
                                         fm->paned, args, n);

    /* IconView inside viewport — wider spacing for labels */
    n = 0;
    XtSetArg(args[n], XtNborderWidth, 0);     n++;
    XtSetArg(args[n], XtNiconSize, 48);        n++;
    XtSetArg(args[n], XtNitemSpacing, 60);     n++;
    XtSetArg(args[n], XtNmultiSelect, True);   n++;
    fm->iconview = XtCreateManagedWidget("iconView", iconViewWidgetClass,
                                         fm->viewport, args, n);
    XtAddCallback(fm->iconview, XtNselectCallback, iconview_callback, fm);
    fm_register_context_menu(fm, fm->iconview);

    fm->listview = NULL;

    /* Status bar */
    Widget statusbar = IswMainWindowGetStatusBar(fm->main_window);
    if (statusbar) {
        n = 0;
        XtSetArg(args[n], XtNlabel, "");           n++;
        XtSetArg(args[n], XtNborderWidth, 0);      n++;
        XtSetArg(args[n], XtNstatusStretch, True);  n++;
        fm->status_label = XtCreateManagedWidget("status", labelWidgetClass,
                                                  statusbar, args, n);
    }
}

void fileview_populate(Fm *fm)
{
    if (fm->view_mode == FM_VIEW_ICON && fm->iconview) {
        /* Build arrays for IconView — must stay alive since
         * IconView stores pointers, not copies */
        static String *labels = NULL;
        static String *icons  = NULL;
        static char **trunc_names = NULL;
        static int trunc_count = 0;

        /* Free previous truncated names */
        for (int i = 0; i < trunc_count; i++)
            free(trunc_names[i]);
        free(trunc_names);
        free(labels);
        free(icons);

        #define MAX_LABEL_LEN 12

        labels = malloc((fm->nentries + 1) * sizeof(String));
        icons  = malloc((fm->nentries + 1) * sizeof(String));
        trunc_names = malloc(fm->nentries * sizeof(char *));
        trunc_count = fm->nentries;

        for (int i = 0; i < fm->nentries; i++) {
            const char *name = fm->entries[i].name;
            if (strlen(name) > MAX_LABEL_LEN) {
                trunc_names[i] = malloc(MAX_LABEL_LEN + 4);
                memcpy(trunc_names[i], name, MAX_LABEL_LEN);
                strcpy(trunc_names[i] + MAX_LABEL_LEN, "...");
                labels[i] = trunc_names[i];
            } else {
                trunc_names[i] = NULL;
                labels[i] = (String)name;
            }
            icons[i] = (String)fm->entries[i].mime_icon;
        }
        IswIconViewSetItems(fm->iconview, labels, icons, fm->nentries);
    } else if (fm->view_mode == FM_VIEW_LIST && fm->listview) {
        /* Build string array for List */
        static String *list_items = NULL;
        free(list_items);
        list_items = malloc((fm->nentries + 1) * sizeof(String));
        for (int i = 0; i < fm->nentries; i++)
            list_items[i] = fm->entries[i].name;
        list_items[fm->nentries] = NULL;
        IswListChange(fm->listview, list_items, fm->nentries, 0, True);
    }

    /* Update status bar */
    if (fm->status_label) {
        char buf[128];
        int ndirs = 0, nfiles = 0;
        for (int i = 0; i < fm->nentries; i++) {
            if (fm->entries[i].is_dir) ndirs++;
            else nfiles++;
        }
        snprintf(buf, sizeof(buf), "%d folders, %d files", ndirs, nfiles);
        Arg args[20];
        XtSetArg(args[0], XtNlabel, buf);
        XtSetValues(fm->status_label, args, 1);
    }
}

void fileview_set_mode(Fm *fm, FmViewMode mode)
{
    if (mode == fm->view_mode) return;

    /* Destroy current view widget */
    if (fm->view_mode == FM_VIEW_ICON && fm->iconview) {
        XtDestroyWidget(fm->iconview);
        fm->iconview = NULL;
    } else if (fm->view_mode == FM_VIEW_LIST && fm->listview) {
        XtDestroyWidget(fm->listview);
        fm->listview = NULL;
    }

    fm->view_mode = mode;

    Arg args[20];
    Cardinal n;

    if (mode == FM_VIEW_ICON) {
        n = 0;
        XtSetArg(args[n], XtNborderWidth, 0); n++;
        fm->iconview = XtCreateManagedWidget("iconView", iconViewWidgetClass,
                                             fm->viewport, args, n);
        XtAddCallback(fm->iconview, XtNselectCallback, iconview_callback, fm);
        fm_register_context_menu(fm, fm->iconview);
    } else {
        static String empty[] = { "(empty)", NULL };
        n = 0;
        XtSetArg(args[n], XtNlist, empty);         n++;
        XtSetArg(args[n], XtNnumberStrings, 1);    n++;
        XtSetArg(args[n], XtNverticalList, True);   n++;
        XtSetArg(args[n], XtNborderWidth, 0);      n++;
        fm->listview = XtCreateManagedWidget("listView", listWidgetClass,
                                             fm->viewport, args, n);
        XtAddCallback(fm->listview, XtNcallback, listview_callback, fm);
        fm_register_context_menu(fm, fm->listview);
    }

    fileview_populate(fm);
}

void fileview_cleanup(Fm *fm)
{
    (void)fm;
    /* Widgets destroyed with app context */
}
