#define _POSIX_C_SOURCE 200809L
/*
 * fileview.c — populate IconView or List from browser entries
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xcb/xcb.h>

/* ---------- double-click tracking ---------- */

static int is_double_click(Fm *fm, int index)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long ms = (now.tv_sec - fm->last_click_time.tv_sec) * 1000 +
              (now.tv_nsec - fm->last_click_time.tv_nsec) / 1000000;

    int dbl = (index == fm->last_click_index &&
               ms < isde_config_double_click_ms());

    fm->last_click_index = index;
    fm->last_click_time = now;

    return dbl;
}

/* ---------- IconView callbacks ---------- */

static void iconview_callback(Widget w, XtPointer client_data,
                              XtPointer call_data)
{
    Fm *fm = (Fm *)client_data;
    IswIconViewCallbackData *d = (IswIconViewCallbackData *)call_data;

    fm_dismiss_context(fm);

    /* Check if triggered by keyboard (Enter/Return) — always open */
    xcb_generic_event_t *ev = XtLastEventProcessed(XtDisplay(w));
    if (ev && ((ev->response_type & ~0x80) == XCB_KEY_PRESS ||
               (ev->response_type & ~0x80) == XCB_KEY_RELEASE)) {
        browser_open_entry(fm, d->index);
        return;
    }

    if (fm->double_click) {
        if (is_double_click(fm, d->index))
            browser_open_entry(fm, d->index);
    } else {
        browser_open_entry(fm, d->index);
    }
}

/* ---------- init / populate ---------- */

void fileview_init(Fm *fm)
{
    /* Viewport for scrolling — below the nav bar, right of places sidebar */
    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNallowVert, True);          n++;
    XtSetArg(args[n], XtNallowHoriz, False);        n++;
    XtSetArg(args[n], XtNuseRight, True);            n++;
    XtSetArg(args[n], XtNborderWidth, 0);            n++;
    XtSetArg(args[n], XtNflexGrow, 1);               n++;
    fm->viewport = XtCreateManagedWidget("viewport", viewportWidgetClass,
                                         fm->hbox, args, n);

    /* IconView inside viewport — wider spacing for labels */
    n = 0;
    XtSetArg(args[n], XtNborderWidth, 0);     n++;
    XtSetArg(args[n], XtNiconSize, 32);        n++;
    XtSetArg(args[n], XtNitemSpacing, 16);     n++;
    XtSetArg(args[n], XtNmultiSelect, True);   n++;
    fm->iconview = XtCreateManagedWidget("iconView", iconViewWidgetClass,
                                         fm->viewport, args, n);
    XtAddCallback(fm->iconview, XtNselectCallback, iconview_callback, fm);
    fm_register_context_menu(fm, fm->iconview);
    fm_install_shortcuts(fm->iconview);

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
    if (fm->iconview) {
        /* Free previous truncated names */
        for (int i = 0; i < fm->fv_trunc_count; i++) {
            free(fm->fv_trunc_names[i]);
        }
        free(fm->fv_trunc_names);
        free(fm->fv_labels);
        free(fm->fv_icons);

        #define MAX_LABEL_LEN 12

        fm->fv_labels = malloc((fm->nentries + 1) * sizeof(String));
        fm->fv_icons  = malloc((fm->nentries + 1) * sizeof(String));
        fm->fv_trunc_names = malloc(fm->nentries * sizeof(char *));
        fm->fv_trunc_count = fm->nentries;

        for (int i = 0; i < fm->nentries; i++) {
            const char *name = fm->entries[i].name;
            if (strlen(name) > MAX_LABEL_LEN) {
                fm->fv_trunc_names[i] = malloc(MAX_LABEL_LEN + 4);
                memcpy(fm->fv_trunc_names[i], name, MAX_LABEL_LEN);
                strcpy(fm->fv_trunc_names[i] + MAX_LABEL_LEN, "...");
                fm->fv_labels[i] = fm->fv_trunc_names[i];
            } else {
                fm->fv_trunc_names[i] = NULL;
                fm->fv_labels[i] = (String)name;
            }
            fm->fv_icons[i] = (String)fm->entries[i].mime_icon;
        }
        IswIconViewSetItems(fm->iconview, fm->fv_labels, fm->fv_icons,
                            fm->nentries);
    }

    /* Update status bar */
    if (fm->status_label) {
        char buf[128];
        int ndirs = 0, nfiles = 0;
        for (int i = 0; i < fm->nentries; i++) {
            if (fm->entries[i].is_dir) {
                ndirs++;
            } else {
                nfiles++;
            }
        }
        snprintf(buf, sizeof(buf), "%d folders, %d files", ndirs, nfiles);
        Arg args[20];
        XtSetArg(args[0], XtNlabel, buf);
        XtSetValues(fm->status_label, args, 1);
    }
}

void fileview_cleanup(Fm *fm)
{
    for (int i = 0; i < fm->fv_trunc_count; i++) {
        free(fm->fv_trunc_names[i]);
    }
    free(fm->fv_trunc_names);
    free(fm->fv_labels);
    free(fm->fv_icons);
    fm->fv_trunc_names = NULL;
    fm->fv_labels = NULL;
    fm->fv_icons = NULL;
    fm->fv_trunc_count = 0;
}
