#define _GNU_SOURCE
/*
 * isde-filechooser.c — File open/save dialog
 */
#include "isde/isde-filechooser.h"
#include "isde/isde-dialog.h"
#include "isde/isde-xdg.h"
#include "isde/isde-theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fnmatch.h>
#include <limits.h>

#include <xcb/xcb.h>
#include <ISW/StringDefs.h>
#include <ISW/Shell.h>
#include <ISW/Form.h>
#include <ISW/Label.h>
#include <ISW/Command.h>
#include <ISW/List.h>
#include <ISW/Viewport.h>
#include <ISW/Text.h>
#include <ISW/Paned.h>
#include <ISW/IswArgMacros.h>

/* ================================================================
 * File chooser state
 * ================================================================ */

typedef struct {
    Widget               shell;
    Widget               path_text;
    Widget               dir_list;
    Widget               file_list;
    Widget               name_text;     /* SAVE mode only, NULL for OPEN */
    Widget               filter_text;
    IsdeFileChooserMode  mode;
    IsdeFileChooserCB    callback;
    void                *user_data;

    char                 cwd[PATH_MAX];
    char                *filter;

    /* Backing arrays for List widgets (must outlive the widgets) */
    String              *dir_names;
    int                  ndir;
    String              *file_names;
    int                  nfile;
} FcState;

/* ================================================================
 * Directory scanning
 * ================================================================ */

static int str_compare(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

static void free_string_array(String **arr, int *count)
{
    if (*arr) {
        for (int i = 0; i < *count; i++)
            free((*arr)[i]);
        free(*arr);
        *arr = NULL;
    }
    *count = 0;
}

static void scan_directory(FcState *fc)
{
    /* Free old entries */
    free_string_array(&fc->dir_names, &fc->ndir);
    free_string_array(&fc->file_names, &fc->nfile);

    DIR *d = opendir(fc->cwd);
    if (!d) return;

    int dir_cap = 32, file_cap = 64;
    fc->dir_names = malloc(dir_cap * sizeof(String));
    fc->file_names = malloc(file_cap * sizeof(String));

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;

        /* Skip hidden files except . and .. */
        if (name[0] == '.' && name[1] != '\0' &&
            !(name[1] == '.' && name[2] == '\0'))
            continue;

        /* Build full path and stat */
        char fullpath[PATH_MAX];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", fc->cwd, name);

        struct stat st;
        if (stat(fullpath, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            if (fc->ndir >= dir_cap) {
                dir_cap *= 2;
                fc->dir_names = realloc(fc->dir_names,
                                        dir_cap * sizeof(String));
            }
            fc->dir_names[fc->ndir++] = strdup(name);
        } else if (S_ISREG(st.st_mode)) {
            /* Apply filter */
            if (fc->filter && fc->filter[0] &&
                fnmatch(fc->filter, name, 0) != 0)
                continue;

            if (fc->nfile >= file_cap) {
                file_cap *= 2;
                fc->file_names = realloc(fc->file_names,
                                         file_cap * sizeof(String));
            }
            fc->file_names[fc->nfile++] = strdup(name);
        }
    }
    closedir(d);

    /* Sort entries (. and .. will sort first naturally) */
    if (fc->ndir > 0)
        qsort(fc->dir_names, fc->ndir, sizeof(String), str_compare);
    if (fc->nfile > 0)
        qsort(fc->file_names, fc->nfile, sizeof(String), str_compare);
}

static void update_lists(FcState *fc)
{
    scan_directory(fc);

    /* Update directory list */
    if (fc->ndir > 0)
        IswListChange(fc->dir_list, fc->dir_names, fc->ndir, 0, True);
    else
        IswListChange(fc->dir_list, NULL, 0, 0, True);

    /* Update file list */
    if (fc->nfile > 0)
        IswListChange(fc->file_list, fc->file_names, fc->nfile, 0, True);
    else
        IswListChange(fc->file_list, NULL, 0, 0, True);

    /* Update path text */
    if (fc->path_text) {
        IswArgBuilder ab = IswArgBuilderInit();
        IswArgString(&ab, fc->cwd);
        IswSetValues(fc->path_text, ab.args, ab.count);
    }
}

/* ================================================================
 * Navigation
 * ================================================================ */

static void navigate_to(FcState *fc, const char *path)
{
    char resolved[PATH_MAX];
    if (!realpath(path, resolved))
        return;

    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode))
        return;

    snprintf(fc->cwd, sizeof(fc->cwd), "%s", resolved);
    update_lists(fc);
}

/* Active file chooser (only one modal dialog at a time) */
static FcState *active_fc;

/* ================================================================
 * Callbacks
 * ================================================================ */

static void fc_free(FcState *fc)
{
    if (!fc) return;
    free_string_array(&fc->dir_names, &fc->ndir);
    free_string_array(&fc->file_names, &fc->nfile);
    free(fc->filter);
    free(fc);
}

static void fc_dismiss(FcState *fc)
{
    Widget shell = fc->shell;
    fc->shell = NULL;
    if (active_fc == fc)
        active_fc = NULL;
    fc_free(fc);
    isde_dialog_dismiss(shell);
}

static void fc_accept(FcState *fc, const char *filename)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", fc->cwd, filename);

    if (fc->callback)
        fc->callback(path, fc->user_data);
    fc_dismiss(fc);
}

/* Directory list: single click navigates */
static void dir_select_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w;
    FcState *fc = (FcState *)cd;
    IswListReturnStruct *ret = (IswListReturnStruct *)call;
    if (!ret || !ret->string) return;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", fc->cwd, ret->string);
    navigate_to(fc, path);
}

/* File list: single click selects (fills name field in SAVE mode) */
static void file_select_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w;
    FcState *fc = (FcState *)cd;
    IswListReturnStruct *ret = (IswListReturnStruct *)call;
    if (!ret || !ret->string) return;

    if (fc->mode == ISDE_FILE_SAVE && fc->name_text) {
        IswArgBuilder ab = IswArgBuilderInit();
        IswArgString(&ab, ret->string);
        IswSetValues(fc->name_text, ab.args, ab.count);
    } else if (fc->mode == ISDE_FILE_OPEN) {
        fc_accept(fc, ret->string);
    }
}

/* OK/Open/Save button */
static void action_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    FcState *fc = (FcState *)cd;

    if (fc->mode == ISDE_FILE_SAVE && fc->name_text) {
        /* Get filename from text field */
        String val = NULL;
        IswArgBuilder ab = IswArgBuilderInit();
        IswArgString(&ab, &val);
        IswGetValues(fc->name_text, ab.args, ab.count);
        if (val && val[0]) {
            fc_accept(fc, val);
            return;
        }
    } else {
        /* OPEN mode: check if a file is highlighted */
        IswListReturnStruct *cur = IswListShowCurrent(fc->file_list);
        if (cur && cur->list_index != XAW_LIST_NONE && cur->string) {
            fc_accept(fc, cur->string);
            return;
        }
    }
}

/* Cancel button */
static void cancel_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    FcState *fc = (FcState *)cd;
    if (fc->callback)
        fc->callback(NULL, fc->user_data);
    fc_dismiss(fc);
}

/* Path text: Enter key navigates */
static void path_action(Widget w, xcb_generic_event_t *ev,
                        String *params, Cardinal *nparams)
{
    (void)ev; (void)params; (void)nparams;
    FcState *fc = active_fc;
    if (!fc) return;

    String val = NULL;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgString(&ab, &val);
    IswGetValues(w, ab.args, ab.count);
    if (val && val[0])
        navigate_to(fc, val);
}

/* Filter text: Enter key re-scans */
static void filter_action(Widget w, xcb_generic_event_t *ev,
                          String *params, Cardinal *nparams)
{
    (void)ev; (void)params; (void)nparams;
    FcState *fc = active_fc;
    if (!fc) return;

    String val = NULL;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgString(&ab, &val);
    IswGetValues(w, ab.args, ab.count);

    free(fc->filter);
    fc->filter = (val && val[0]) ? strdup(val) : NULL;
    update_lists(fc);
}

/* ================================================================
 * Action registration
 * ================================================================ */

static IswActionsRec fc_actions[] = {
    {"isde-fc-path-go",   path_action},
    {"isde-fc-filter-go", filter_action},
};

static void ensure_fc_actions(Widget w)
{
    static int registered;
    if (!registered) {
        IswAppAddActions(IswWidgetToApplicationContext(w),
                        fc_actions, IswNumber(fc_actions));
        registered = 1;
    }
}

/* ================================================================
 * Public API
 * ================================================================ */

Widget isde_filechooser_show(Widget parent, const char *title,
                             IsdeFileChooserMode mode,
                             const char *initial_dir,
                             const char *filter,
                             IsdeFileChooserCB callback, void *data)
{
    ensure_fc_actions(parent);

    FcState *fc = calloc(1, sizeof(*fc));
    fc->mode = mode;
    fc->callback = callback;
    fc->user_data = data;
    fc->filter = filter ? strdup(filter) : NULL;

    /* Set initial directory */
    if (initial_dir && initial_dir[0]) {
        char resolved[PATH_MAX];
        if (realpath(initial_dir, resolved))
            snprintf(fc->cwd, sizeof(fc->cwd), "%s", resolved);
        else
            snprintf(fc->cwd, sizeof(fc->cwd), "%s",
                     getenv("HOME"));
    } else {
        snprintf(fc->cwd, sizeof(fc->cwd), "%s",
                 getenv("HOME"));
    }

    /* Dialog shell */
    fc->shell = isde_dialog_create_shell(parent, "fileChooserShell",
                                         title, 500, 400);

    active_fc = fc;

    IswArgBuilder ab = IswArgBuilderInit();

    /* Main form */
    IswArgDefaultDistance(&ab, 8);
    IswArgBorderWidth(&ab, 0);
    Widget form = IswCreateManagedWidget("fcMainForm", formWidgetClass,
                                        fc->shell, ab.args, ab.count);

    /* --- Row 1: Location label + path text --- */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Location:");
    IswArgBorderWidth(&ab, 0);
    IswArgTop(&ab, IswChainTop);
    IswArgBottom(&ab, IswChainTop);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    Widget loc_label = IswCreateManagedWidget("locLabel", labelWidgetClass,
                                              form, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgString(&ab, fc->cwd);
    IswArgFromHoriz(&ab, loc_label);
    IswArgEditType(&ab, IswtextEdit);
    IswArgTop(&ab, IswChainTop);
    IswArgBottom(&ab, IswChainTop);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainRight);
    fc->path_text = IswCreateManagedWidget("pathText", textWidgetClass,
                                           form, ab.args, ab.count);
    IswOverrideTranslations(fc->path_text, IswParseTranslationTable(
        "<Key>Return: isde-fc-path-go()\n"));

    /* --- Row 2: Directory + File panes --- */

    /* Load theme for pane toning */
    const IsdeColorScheme *scheme = isde_theme_current();

    /* Directory list viewport (left pane) */
    int pane_h = 250;
    int dir_w  = 160;

    IswArgBuilderReset(&ab);
    IswArgFromVert(&ab, loc_label);
    IswArgAllowVert(&ab, True);
    IswArgUseRight(&ab, True);
    IswArgForceBars(&ab, True);
    IswArgWidth(&ab, dir_w);
    IswArgHeight(&ab, pane_h);
    IswArgTop(&ab, IswChainTop);
    IswArgBottom(&ab, IswChainBottom);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    if (scheme) {
        IswArgBackground(&ab, scheme->bg);
    }
    Widget dir_vp = IswCreateManagedWidget("dirViewport", viewportWidgetClass,
                                           form, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgVerticalList(&ab, True);
    IswArgForceColumns(&ab, True);
    IswArgDefaultColumns(&ab, 1);
    IswArgBorderWidth(&ab, 0);
    if (scheme) {
        IswArgBackground(&ab, scheme->bg);
        IswArgForeground(&ab, scheme->fg);
    }
    fc->dir_list = IswCreateManagedWidget("dirList", listWidgetClass,
                                          dir_vp, ab.args, ab.count);
    IswAddCallback(fc->dir_list, IswNcallback, dir_select_cb, fc);

    /* File list viewport (right pane) */
    IswArgBuilderReset(&ab);
    IswArgFromVert(&ab, loc_label);
    IswArgFromHoriz(&ab, dir_vp);
    IswArgAllowVert(&ab, True);
    IswArgUseRight(&ab, True);
    IswArgForceBars(&ab, True);
    IswArgHeight(&ab, pane_h);
    IswArgTop(&ab, IswChainTop);
    IswArgBottom(&ab, IswChainBottom);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainRight);
    if (scheme) {
        IswArgBackground(&ab, scheme->bg_light);
    }
    Widget file_vp = IswCreateManagedWidget("fileViewport", viewportWidgetClass,
                                            form, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgVerticalList(&ab, True);
    IswArgForceColumns(&ab, True);
    IswArgDefaultColumns(&ab, 1);
    IswArgBorderWidth(&ab, 0);
    if (scheme) {
        IswArgBackground(&ab, scheme->bg_light);
        IswArgForeground(&ab, scheme->fg);
    }
    fc->file_list = IswCreateManagedWidget("fileList", listWidgetClass,
                                           file_vp, ab.args, ab.count);
    IswAddCallback(fc->file_list, IswNcallback, file_select_cb, fc);

    /* Track the last widget above the bottom rows */
    Widget bottom_anchor = dir_vp;

    /* --- Row 3 (SAVE mode only): Filename --- */
    if (mode == ISDE_FILE_SAVE) {
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, "Name:");
        IswArgBorderWidth(&ab, 0);
        IswArgFromVert(&ab, dir_vp);
        IswArgTop(&ab, IswChainBottom);
        IswArgBottom(&ab, IswChainBottom);
        IswArgLeft(&ab, IswChainLeft);
        IswArgRight(&ab, IswChainLeft);
        Widget name_label = IswCreateManagedWidget("nameLabel",
                                                   labelWidgetClass,
                                                   form, ab.args, ab.count);

        IswArgBuilderReset(&ab);
        IswArgString(&ab, "");
        IswArgFromVert(&ab, dir_vp);
        IswArgFromHoriz(&ab, name_label);
        IswArgEditType(&ab, IswtextEdit);
        IswArgTop(&ab, IswChainBottom);
        IswArgBottom(&ab, IswChainBottom);
        IswArgLeft(&ab, IswChainLeft);
        IswArgRight(&ab, IswChainRight);
        fc->name_text = IswCreateManagedWidget("nameText",
                                               textWidgetClass,
                                               form, ab.args, ab.count);
        bottom_anchor = name_label;
    }

    /* --- Row 4: Filter --- */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Filter:");
    IswArgBorderWidth(&ab, 0);
    IswArgFromVert(&ab, bottom_anchor);
    IswArgTop(&ab, IswChainBottom);
    IswArgBottom(&ab, IswChainBottom);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    Widget filter_label = IswCreateManagedWidget("filterLabel",
                                                 labelWidgetClass,
                                                 form, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgString(&ab, filter ? filter : "");
    IswArgFromVert(&ab, bottom_anchor);
    IswArgFromHoriz(&ab, filter_label);
    IswArgEditType(&ab, IswtextEdit);
    IswArgTop(&ab, IswChainBottom);
    IswArgBottom(&ab, IswChainBottom);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainRight);
    fc->filter_text = IswCreateManagedWidget("filterText",
                                             textWidgetClass,
                                             form, ab.args, ab.count);
    IswOverrideTranslations(fc->filter_text, IswParseTranslationTable(
        "<Key>Return: isde-fc-filter-go()\n"));

    /* --- Row 5: Buttons --- */
    const char *action_label = (mode == ISDE_FILE_SAVE) ? "Save" : "Open";
    IsdeDialogButton btns[2] = {
        { action_label, action_cb, fc },
        { "Cancel",     cancel_cb, fc },
    };
    isde_dialog_add_buttons(form, filter_label,
                            500 - 8 * 2,
                            btns, 2);

    /* Initial scan */
    scan_directory(fc);
    if (fc->ndir > 0)
        IswListChange(fc->dir_list, fc->dir_names, fc->ndir, 0, True);
    if (fc->nfile > 0)
        IswListChange(fc->file_list, fc->file_names, fc->nfile, 0, True);

    isde_dialog_popup(fc->shell, IswGrabExclusive);
    return fc->shell;
}
