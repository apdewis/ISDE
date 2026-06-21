#define _POSIX_C_SOURCE 200809L
/*
 * dialogs.c — rename, delete, properties, set-default, and progress dialogs
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>

#include <time.h>

#include <ISW/IswArgMacros.h>
#include <ISW/Toggle.h>
#include <ISW/ProgressBar.h>
#include <isw-graph/GraphLine.h>
#include "isde-dialog.h"

/* ---------- rename dialog ---------- */

static void rename_result_cb(IsdeDialogResult result,
                             const char *text, void *data)
{
    Fm *fm = (Fm *)data;
    fm->rename_shell = NULL;
    if (result != ISDE_DIALOG_OK || !text || !text[0])
        return;
    if (fm->rename_index >= 0 && fm->rename_index < fm->nentries) {
        fileops_rename(fm, fm->entries[fm->rename_index].full_path, text);
        fm_refresh(fm);
    }
}

void show_rename_dialog(Fm *fm)
{
    int sel = fileview_get_selected(fm);
    if (sel < 0 || sel >= fm->nentries)
        return;

    fm->rename_index = sel;
    isde_dialog_dismiss(fm->rename_shell);
    fm->rename_shell = isde_dialog_input(fm->toplevel, "Rename",
                                         "Rename:",
                                         fm->entries[sel].name,
                                         rename_result_cb, fm);
}

/* ---------- delete with confirmation ---------- */

typedef struct {
    Fm  *fm;
    int  permanent;
} FmDeleteCtx;

static void delete_result_cb(IsdeDialogResult result, void *data)
{
    FmDeleteCtx *dctx = (FmDeleteCtx *)data;
    Fm *fm = dctx->fm;
    int permanent = dctx->permanent;
    free(dctx);
    fm->delete_shell = NULL;

    if (result != ISDE_DIALOG_OK)
        return;

    int *indices = NULL;
    int nsel = fileview_get_selected_items(fm, &indices);
    if (nsel > 0) {
        char **paths = malloc(nsel * sizeof(char *));
        int npaths = 0;
        for (int i = 0; i < nsel; i++) {
            int idx = indices[i];
            if (idx >= 0 && idx < fm->nentries)
                paths[npaths++] = fm->entries[idx].full_path;
        }
        if (npaths > 0) {
            if (permanent)
                jobqueue_submit_delete(fm->app_state, fm, paths, npaths);
            else
                jobqueue_submit_trash(fm->app_state, fm, paths, npaths);
        }
        free(paths);
    }
    free(indices);
}

static void fm_delete_confirm(Fm *fm, int permanent)
{
    int *indices = NULL;
    int nsel = fileview_get_selected_items(fm, &indices);
    if (nsel <= 0) {
        free(indices);
        return;
    }

    char *trash_path = fileops_trash_path();
    int in_trash = (strncmp(fm->cwd, trash_path, strlen(trash_path)) == 0);
    free(trash_path);

    if (in_trash)
        permanent = 1;

    char msg[256];
    if (nsel == 1) {
        int idx = indices[0];
        const char *name = (idx >= 0 && idx < fm->nentries)
                           ? fm->entries[idx].name : "selected item";
        if (permanent)
            snprintf(msg, sizeof(msg), "Permanently delete \"%s\"?", name);
        else
            snprintf(msg, sizeof(msg), "Move \"%s\" to Trash?", name);
    } else {
        if (permanent)
            snprintf(msg, sizeof(msg), "Permanently delete %d items?", nsel);
        else
            snprintf(msg, sizeof(msg), "Move %d items to Trash?", nsel);
    }
    free(indices);

    isde_dialog_dismiss(fm->delete_shell);

    FmDeleteCtx *dctx = malloc(sizeof(*dctx));
    dctx->fm = fm;
    dctx->permanent = permanent;

    fm->delete_shell = isde_dialog_confirm(fm->toplevel, "Confirm",
                                           msg,
                                           permanent ? "Delete" : "Move",
                                           delete_result_cb, dctx);
}

void fm_delete_selected(Fm *fm)
{
    fm_delete_confirm(fm, 0);
}

void fm_delete_selected_permanent(Fm *fm)
{
    fm_delete_confirm(fm, 1);
}

/* ---------- empty trash ---------- */

static void empty_trash_result_cb(IsdeDialogResult result, void *data)
{
    Fm *fm = (Fm *)data;
    fm->empty_trash_shell = NULL;
    if (result == ISDE_DIALOG_OK)
        jobqueue_submit_empty_trash(fm->app_state, fm);
}

void ctx_empty_trash(Fm *fm)
{
    isde_dialog_dismiss(fm->empty_trash_shell);
    fm->empty_trash_shell = isde_dialog_confirm(
        fm->toplevel, "Empty Trash",
        "Permanently delete all items in Trash?",
        "Empty Trash", empty_trash_result_cb, fm);
}

/* ---------- open terminal ---------- */

void ctx_open_terminal(Fm *fm)
{
    const char *term = isde_desktop_get_terminal();
    pid_t pid = fork();
    if (pid == 0) {
        execlp(term, term, "--working-directory", fm->cwd, (char *)NULL);
        _exit(127);
    }
}

/* ---------- properties dialog ---------- */

typedef struct {
    Fm     *fm;
    Widget  shell;
    Widget  toggles[9]; /* owner rwx, group rwx, others rwx */
    char   *path;
} PropsCtx;

static void props_apply_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    PropsCtx *ctx = (PropsCtx *)cd;

    mode_t mode = 0;
    static const mode_t bits[] = {
        S_IRUSR, S_IWUSR, S_IXUSR,
        S_IRGRP, S_IWGRP, S_IXGRP,
        S_IROTH, S_IWOTH, S_IXOTH,
    };
    for (int i = 0; i < 9; i++) {
        Boolean state = False;
        Arg args[1];
        IswSetArg(args[0], IswNstate, &state);
        IswGetValues(ctx->toggles[i], args, 1);
        if (state)
            mode |= bits[i];
    }

    if (chmod(ctx->path, mode) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Cannot change permissions:\n%s",
                 strerror(errno));
        isde_dialog_message(ctx->shell, "Error", msg, NULL, NULL);
    }
}

static void props_close_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    PropsCtx *ctx = (PropsCtx *)cd;
    ctx->fm->props_shell = NULL;
    isde_dialog_dismiss(ctx->shell);
    free(ctx->path);
    free(ctx);
}

void show_properties_dialog(Fm *fm)
{
    int sel = fileview_get_selected(fm);
    if (sel < 0 || sel >= fm->nentries)
        return;

    FmEntry *e = &fm->entries[sel];
    struct stat st;
    if (stat(e->full_path, &st) != 0) {
        isde_dialog_message(fm->toplevel, "Error",
                            "Cannot read file information.", NULL, NULL);
        return;
    }

    isde_dialog_dismiss(fm->props_shell);

    PropsCtx *ctx = calloc(1, sizeof(*ctx));
    ctx->fm = fm;
    ctx->path = strdup(e->full_path);

    ctx->shell = isde_dialog_create_shell(fm->toplevel, "propsShell",
                                          "Properties", 420, 480);
    fm->props_shell = ctx->shell;

    Widget form = IswCreateManagedWidget("form", formWidgetClass,
                                        ctx->shell, NULL, 0);

    /* --- info rows --- */
    static const char *field_names[] = {
        "Name:", "Location:", "Type:", "Size:", "Modified:",
        "Owner:", "Group:"
    };
    #define NFIELDS 7
    #define LABEL_W 80

    /* Prepare value strings */
    char size_str[32];
    if (e->is_dir) {
        snprintf(size_str, sizeof(size_str), "--");
    } else {
        if (st.st_size < 1024)
            snprintf(size_str, sizeof(size_str), "%ld B", (long)st.st_size);
        else if (st.st_size < 1024 * 1024)
            snprintf(size_str, sizeof(size_str), "%.1f KiB",
                     st.st_size / 1024.0);
        else if (st.st_size < 1024L * 1024 * 1024)
            snprintf(size_str, sizeof(size_str), "%.1f MiB",
                     st.st_size / (1024.0 * 1024));
        else if (st.st_size < 1024LL * 1024 * 1024 * 1024)
            snprintf(size_str, sizeof(size_str), "%.1f GiB",
                     st.st_size / (1024.0 * 1024 * 1024));
        else
            snprintf(size_str, sizeof(size_str), "%.1f TiB",
                     st.st_size / (1024.0 * 1024 * 1024 * 1024));
    }

    char time_str[64];
    struct tm *tm = localtime(&st.st_mtime);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M", tm);

    /* Dirname */
    char *dir = strdup(e->full_path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) *slash = '\0';
    else if (slash) dir[1] = '\0';

    /* Owner / group */
    char owner_str[64], group_str[64];
    struct passwd *pw = getpwuid(st.st_uid);
    if (pw)
        snprintf(owner_str, sizeof(owner_str), "%s", pw->pw_name);
    else
        snprintf(owner_str, sizeof(owner_str), "%u", st.st_uid);

    struct group *gr = getgrgid(st.st_gid);
    if (gr)
        snprintf(group_str, sizeof(group_str), "%s", gr->gr_name);
    else
        snprintf(group_str, sizeof(group_str), "%u", st.st_gid);

    const char *type_str = e->is_dir ? "Folder" : "File";
    const char *dot = e->is_dir ? NULL : strrchr(e->name, '.');
    char type_buf[64];
    if (dot && dot[1]) {
        snprintf(type_buf, sizeof(type_buf), "%s file", dot + 1);
        type_str = type_buf;
    }

    const char *values[] = {
        e->name, dir, type_str, size_str, time_str,
        owner_str, group_str
    };

    Widget prev_row = NULL;
    IswArgBuilder ab = IswArgBuilderInit();
    for (int i = 0; i < NFIELDS; i++) {
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, field_names[i]);
        IswArgBorderWidth(&ab, 0);
        IswArgWidth(&ab, LABEL_W);
        IswArgJustify(&ab, IswJustifyRight);
        IswArgResize(&ab, False);
        IswArgLeft(&ab, IswChainLeft);
        IswArgRight(&ab, IswChainLeft);
        IswArgTop(&ab, IswChainTop);
        IswArgBottom(&ab, IswChainTop);
        if (prev_row)
            IswArgFromVert(&ab, prev_row);
        Widget lbl = IswCreateManagedWidget("fieldLabel", labelWidgetClass,
                                            form, ab.args, ab.count);

        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, values[i]);
        IswArgBorderWidth(&ab, 0);
        IswArgFromHoriz(&ab, lbl);
        IswArgLeft(&ab, IswChainLeft);
        IswArgRight(&ab, IswChainRight);
        IswArgTop(&ab, IswChainTop);
        IswArgBottom(&ab, IswChainTop);
        IswArgJustify(&ab, IswJustifyLeft);
        if (prev_row)
            IswArgFromVert(&ab, prev_row);
        prev_row = IswCreateManagedWidget("fieldValue", labelWidgetClass,
                                          form, ab.args, ab.count);
    }

    free(dir);

    /* --- permissions section --- */
    static const char *col_headers[] = { "Read", "Write", "Execute" };
    static const char *row_headers[] = { "Owner", "Group", "Others" };
    static const mode_t bits[] = {
        S_IRUSR, S_IWUSR, S_IXUSR,
        S_IRGRP, S_IWGRP, S_IXGRP,
        S_IROTH, S_IWOTH, S_IXOTH,
    };

    /* Section label */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Permissions");
    IswArgBorderWidth(&ab, 0);
    IswArgFromVert(&ab, prev_row);
    IswArgVertDistance(&ab, 16);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    IswArgTop(&ab, IswChainTop);
    IswArgBottom(&ab, IswChainTop);
    Widget section_lbl = IswCreateManagedWidget("sectionHd", labelWidgetClass,
                                                form, ab.args, ab.count);

    /* Column headers */
    Widget col_prev = NULL;
    Widget col_hdrs[3];
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "");
    IswArgBorderWidth(&ab, 0);
    IswArgWidth(&ab, LABEL_W);
    IswArgFromVert(&ab, section_lbl);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    IswArgTop(&ab, IswChainTop);
    IswArgBottom(&ab, IswChainTop);
    IswArgResize(&ab, False);
    col_prev = IswCreateManagedWidget("spacer", labelWidgetClass,
                                      form, ab.args, ab.count);

    for (int c = 0; c < 3; c++) {
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, col_headers[c]);
        IswArgBorderWidth(&ab, 0);
        IswArgFromHoriz(&ab, col_prev);
        IswArgFromVert(&ab, section_lbl);
        IswArgLeft(&ab, IswChainLeft);
        IswArgRight(&ab, IswChainLeft);
        IswArgTop(&ab, IswChainTop);
        IswArgBottom(&ab, IswChainTop);
        col_hdrs[c] = IswCreateManagedWidget("colHdr", labelWidgetClass,
                                              form, ab.args, ab.count);
        col_prev = col_hdrs[c];
    }

    /* Toggle grid: 3 rows x 3 cols */
    Widget row_above = col_hdrs[0];
    for (int r = 0; r < 3; r++) {
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, row_headers[r]);
        IswArgBorderWidth(&ab, 0);
        IswArgWidth(&ab, LABEL_W);
        IswArgJustify(&ab, IswJustifyRight);
        IswArgResize(&ab, False);
        IswArgFromVert(&ab, row_above);
        IswArgLeft(&ab, IswChainLeft);
        IswArgRight(&ab, IswChainLeft);
        IswArgTop(&ab, IswChainTop);
        IswArgBottom(&ab, IswChainTop);
        Widget row_lbl = IswCreateManagedWidget("rowLabel", labelWidgetClass,
                                                form, ab.args, ab.count);

        Widget hprev = row_lbl;
        for (int c = 0; c < 3; c++) {
            int idx = r * 3 + c;
            IswArgBuilderReset(&ab);
            IswArgLabel(&ab, "");
            IswArgBorderWidth(&ab, 1);
            IswArgFromHoriz(&ab, hprev);
            IswArgFromVert(&ab, row_above);
            IswArgLeft(&ab, IswChainLeft);
            IswArgRight(&ab, IswChainLeft);
            IswArgTop(&ab, IswChainTop);
            IswArgBottom(&ab, IswChainTop);
            if ((st.st_mode & bits[idx]))
                IswArgState(&ab, True);
            ctx->toggles[idx] = IswCreateManagedWidget(
                "permToggle", toggleWidgetClass, form, ab.args, ab.count);
            hprev = ctx->toggles[idx];
        }
        row_above = row_lbl;
    }

    /* Buttons */
    IsdeDialogButton btns[] = {
        { "Apply", props_apply_cb, ctx },
        { "Close", props_close_cb, ctx },
    };
    isde_dialog_add_buttons(form, row_above, 420, btns, 2);

    isde_dialog_popup(ctx->shell, IswGrabExclusive);

    #undef NFIELDS
    #undef LABEL_W
}

/* ---------- "Set Default Application" dialog ---------- */

typedef struct {
    Fm     *fm;
    Widget  shell;
    Widget  list;
    int    *de_indices;   /* desktop entry indices for each list row */
    int     nentries;
    char   *mime;
    String *labels;       /* kept alive for List widget */
} SetDefaultCtx;

static void set_default_ok_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    SetDefaultCtx *ctx = (SetDefaultCtx *)cd;
    Fm *fm = ctx->fm;
    FmApp *app = fm->app_state;

    IswListReturnStruct *ret = IswListShowCurrent(ctx->list);
    if (ret && ret->list_index >= 0 && ret->list_index < ctx->nentries) {
        int de_idx = ctx->de_indices[ret->list_index];
        const char *desktop_id = isde_desktop_id(app->desktop_entries[de_idx]);
        if (desktop_id)
            isde_mime_set_default(ctx->mime, desktop_id);
    }

    fm->set_default_shell = NULL;
    isde_dialog_dismiss(ctx->shell);
    free(ctx->labels);
    free(ctx->de_indices);
    free(ctx->mime);
    free(ctx);
}

static void set_default_cancel_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    SetDefaultCtx *ctx = (SetDefaultCtx *)cd;
    ctx->fm->set_default_shell = NULL;
    isde_dialog_dismiss(ctx->shell);
    free(ctx->labels);
    free(ctx->de_indices);
    free(ctx->mime);
    free(ctx);
}

void ctx_set_default(Fm *fm)
{
    if (!fm->ow_mime || fm->ow_count == 0)
        return;

    isde_dialog_dismiss(fm->set_default_shell);

    SetDefaultCtx *ctx = calloc(1, sizeof(*ctx));
    ctx->fm = fm;
    ctx->mime = strdup(fm->ow_mime);
    ctx->nentries = fm->ow_count;
    ctx->de_indices = malloc(fm->ow_count * sizeof(int));
    memcpy(ctx->de_indices, fm->ow_indices, fm->ow_count * sizeof(int));

    ctx->shell = isde_dialog_create_shell(fm->toplevel, "setDefaultShell",
                                          "Set Default Application", 300, 250);

    FmApp *app = fm->app_state;
    ctx->labels = malloc((ctx->nentries + 1) * sizeof(String));
    for (int i = 0; i < ctx->nentries; i++)
        ctx->labels[i] = (String)isde_desktop_name(app->desktop_entries[ctx->de_indices[i]]);
    ctx->labels[ctx->nentries] = NULL;

    IswArgBuilder ab = IswArgBuilderInit();
    Widget form = IswCreateManagedWidget("form", formWidgetClass,
                                        ctx->shell, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Select application:");
    IswArgBorderWidth(&ab, 0);
    IswArgTop(&ab, IswChainTop);
    IswArgBottom(&ab, IswChainTop);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainRight);
    Widget prompt = IswCreateManagedWidget("prompt", labelWidgetClass,
                                           form, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgList(&ab, ctx->labels);
    IswArgNumberStrings(&ab, ctx->nentries);
    IswArgDefaultColumns(&ab, 1);
    IswArgForceColumns(&ab, True);
    IswArgVerticalList(&ab, True);
    IswArgFromVert(&ab, prompt);
    IswArgTop(&ab, IswChainTop);
    IswArgBottom(&ab, IswChainBottom);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainRight);
    ctx->list = IswCreateManagedWidget("appList", listWidgetClass,
                                       form, ab.args, ab.count);

    if (fm->ow_default >= 0)
        IswListHighlight(ctx->list, fm->ow_default);

    IsdeDialogButton btns[2] = {
        { "Apply", set_default_ok_cb,     ctx },
        { "Cancel",      set_default_cancel_cb, ctx },
    };
    isde_dialog_add_buttons(form, ctx->list, 300, btns, 2);

    fm->set_default_shell = ctx->shell;
    isde_dialog_popup(ctx->shell, IswGrabExclusive);
}

/* ---------- progress dialog ---------- */

#define PROGRESS_SHOW_DELAY_MS 500
#define POLL_INTERVAL_MS       200

typedef struct IsdeProgress {
    Widget       shell;
    Widget       bar;
    Widget       label;
    Widget       file_bar;
    Widget       file_label;
    Widget       rate_label;
    Widget       graph;
    struct kdata *graph_data;
    struct timespec start_time;
    long long    prev_bytes;
    double       prev_time;
    double       smooth_rate;
    IswIntervalId show_timer;
    IswAppContext app;
    Widget       parent;
    const char  *title;
    IswCallbackProc cancel_cb;
    void        *cancel_data;
    int          last_pct;
    int          last_file_pct;
    char         last_msg[128];
    char         last_file_msg[128];
    char         last_rate_msg[64];
} IsdeProgress;

static void progress_shell_destroyed(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    IsdeProgress *p = (IsdeProgress *)cd;
    p->shell = NULL;
    p->bar = NULL;
    p->label = NULL;
    p->file_bar = NULL;
    p->file_label = NULL;
    p->rate_label = NULL;
    p->graph = NULL;
}

static void progress_create_dialog(IsdeProgress *p)
{
    p->shell = isde_dialog_create_shell(p->parent, "progressShell",
                                        p->title, 350, 320);
    IswAddCallback(p->shell, IswNdestroyCallback,
                   progress_shell_destroyed, p);

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgOrientation(&ab, IswOrientVertical);
    IswArgBorderWidth(&ab, 0);
    Widget vbox = IswCreateManagedWidget("progressBox", flexBoxWidgetClass,
                                         p->shell, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "");
    IswArgBorderWidth(&ab, 0);
    IswArgJustify(&ab, IswJustifyLeft);
    IswArgResize(&ab, False);
    p->label = IswCreateManagedWidget("progressLabel", labelWidgetClass,
                                      vbox, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgValue(&ab, 0);
    IswArgBorderWidth(&ab, 0);
    IswArgFlexGrow(&ab, 1);
    p->bar = IswCreateManagedWidget("progressBar", progressBarWidgetClass,
                                    vbox, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "");
    IswArgBorderWidth(&ab, 0);
    IswArgJustify(&ab, IswJustifyLeft);
    IswArgResize(&ab, False);
    p->file_label = IswCreateManagedWidget("fileLabel", labelWidgetClass,
                                           vbox, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgValue(&ab, 0);
    IswArgBorderWidth(&ab, 0);
    IswArgFlexGrow(&ab, 1);
    p->file_bar = IswCreateManagedWidget("fileBar", progressBarWidgetClass,
                                         vbox, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "");
    IswArgBorderWidth(&ab, 0);
    IswArgJustify(&ab, IswJustifyLeft);
    IswArgResize(&ab, False);
    p->rate_label = IswCreateManagedWidget("rateLabel", labelWidgetClass,
                                           vbox, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgWidth(&ab, 320);
    IswArgHeight(&ab, 100);
    IswArgBorderWidth(&ab, 0);
    IswArgFlexGrow(&ab, 2);
    ISW_ARG(&ab, IswGraphNshowGrid, True);
    ISW_ARG(&ab, IswGraphNyAxisLabel, "MB/s");
    ISW_ARG(&ab, IswGraphNshowTicLabels, True);
    ISW_ARG(&ab, IswGraphNyTics, 4);
    ISW_ARG(&ab, IswGraphNxTics, 0);
    ISW_ARG(&ab, IswGraphNshowBorder, False);
    p->graph = IswCreateManagedWidget("rateGraph", graphLineWidgetClass,
                                      vbox, ab.args, ab.count);

    struct kplotcfg *pcfg = IswGraphGetPlotCfg(p->graph);
    pcfg->extrema |= EXTREMA_YMIN;
    pcfg->extrema_ymin = 0.0;
    pcfg->ticlabel = TICLABEL_LEFT;

    p->graph_data = kdata_vector_alloc(64);
    struct kdatacfg dcfg;
    kdatacfg_defaults(&dcfg);
    dcfg.line.clr.type = KPLOTCTYPE_RGBA;
    dcfg.line.clr.rgba[0] = 0.2;
    dcfg.line.clr.rgba[1] = 0.5;
    dcfg.line.clr.rgba[2] = 0.8;
    dcfg.line.clr.rgba[3] = 1.0;
    dcfg.line.sz = 2.0;
    IswGraphAttachData(p->graph, p->graph_data, KPLOT_LINES, &dcfg);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Cancel");
    IswArgWidth(&ab, 80);
    IswArgInternalWidth(&ab, 8);
    IswArgInternalHeight(&ab, 8);
    IswArgFlexAlign(&ab, IswFlexAlignEnd);
    Widget cancel = IswCreateManagedWidget("cancelBtn", commandWidgetClass,
                                           vbox, ab.args, ab.count);
    if (p->cancel_cb)
        IswAddCallback(cancel, IswNcallback, p->cancel_cb, p->cancel_data);

    clock_gettime(CLOCK_MONOTONIC, &p->start_time);
    p->prev_time = -1.0;

    isde_dialog_popup(p->shell, IswGrabNone);
}

static void progress_show_delay_cb(IswPointer closure, IswIntervalId *id)
{
    (void)id;
    IsdeProgress *p = (IsdeProgress *)closure;
    p->show_timer = 0;
    progress_create_dialog(p);
}

static IsdeProgress *progress_dialog_create(Widget parent, const char *title,
                                            IswAppContext app,
                                            IswCallbackProc cancel_cb,
                                            void *data)
{
    IsdeProgress *p = calloc(1, sizeof(*p));
    p->parent = parent;
    p->title = title;
    p->app = app;
    p->cancel_cb = cancel_cb;
    p->cancel_data = data;

    p->show_timer = IswAppAddTimeOut(app, PROGRESS_SHOW_DELAY_MS,
                                    progress_show_delay_cb, p);
    return p;
}

static void progress_dialog_update(IsdeProgress *p, int percent,
                                   const char *message)
{
    if (!p || !p->shell) return;

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    if (p->bar && percent != p->last_pct) {
        IswArgBuilder ab = IswArgBuilderInit();
        IswArgValue(&ab, percent);
        IswSetValues(p->bar, ab.args, ab.count);
        p->last_pct = percent;
    }
    if (p->label && message && strcmp(message, p->last_msg) != 0) {
        IswArgBuilder ab = IswArgBuilderInit();
        IswArgLabel(&ab, message);
        IswSetValues(p->label, ab.args, ab.count);
        snprintf(p->last_msg, sizeof(p->last_msg), "%s", message);
    }
}

static void progress_dialog_update_file(IsdeProgress *p, int percent,
                                        const char *message)
{
    if (!p || !p->shell) return;

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    if (p->file_bar && percent != p->last_file_pct) {
        IswArgBuilder ab = IswArgBuilderInit();
        IswArgValue(&ab, percent);
        IswSetValues(p->file_bar, ab.args, ab.count);
        p->last_file_pct = percent;
    }
    if (p->file_label && message &&
        strcmp(message, p->last_file_msg) != 0) {
        IswArgBuilder ab = IswArgBuilderInit();
        IswArgLabel(&ab, message);
        IswSetValues(p->file_label, ab.args, ab.count);
        snprintf(p->last_file_msg, sizeof(p->last_file_msg), "%s", message);
    }
}

static void progress_dialog_destroy(IsdeProgress *p)
{
    if (!p) return;

    if (p->show_timer) {
        IswRemoveTimeOut(p->show_timer);
        p->show_timer = 0;
    }
    if (p->shell) {
        isde_dialog_dismiss(p->shell);
        p->shell = NULL;
        p->bar = NULL;
        p->label = NULL;
        p->file_bar = NULL;
        p->file_label = NULL;
        p->rate_label = NULL;
        p->graph = NULL;
    }
    if (p->graph_data) {
        kdata_destroy(p->graph_data);
        p->graph_data = NULL;
    }
    free(p);
}

/* ---------- progress polling (timer-driven) ---------- */

static const char *job_type_verb(FmJobType type)
{
    switch (type) {
    case FM_JOB_COPY:        return "Copying";
    case FM_JOB_MOVE:        return "Moving";
    case FM_JOB_DELETE:      return "Deleting";
    case FM_JOB_TRASH:       return "Trashing";
    case FM_JOB_EMPTY_TRASH: return "Emptying trash";
    }
    return "Processing";
}

static void cancel_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    FmJob *job = (FmJob *)cd;
    atomic_store(&job->cancelled, 1);
}

static void poll_timer_cb(IswPointer closure, IswIntervalId *id)
{
    (void)id;
    FmJob *job = (FmJob *)closure;
    job->progress_timer = 0;

    if (atomic_load(&job->finished))
        return;

    int done  = atomic_load(&job->files_done);
    int total = atomic_load(&job->files_total);
    int pct   = (total > 0) ? (done * 100 / total) : 0;
    if (pct > 100) pct = 100;

    char buf[128];
    int cur = done + 1;
    if (cur > total) cur = total;
    snprintf(buf, sizeof(buf), "%s file %d of %d...",
             job_type_verb(job->type), cur, total);

    progress_dialog_update(job->progress, pct, buf);

    if (job->type == FM_JOB_COPY || job->type == FM_JOB_MOVE) {
        long long cb = atomic_load(&job->cur_bytes_done);
        long long ct = atomic_load(&job->cur_bytes_total);
        int fpct = (ct > 0) ? (int)(cb * 100 / ct) : 0;
        if (fpct > 100) fpct = 100;

        char fbuf[128];
        if (ct >= 1024LL * 1024) {
            snprintf(fbuf, sizeof(fbuf), "%.1f / %.1f MB",
                     cb / (1024.0 * 1024.0), ct / (1024.0 * 1024.0));
        } else if (ct >= 1024) {
            snprintf(fbuf, sizeof(fbuf), "%.1f / %.1f KB",
                     cb / 1024.0, ct / 1024.0);
        } else {
            snprintf(fbuf, sizeof(fbuf), "%lld / %lld bytes", cb, ct);
        }
        progress_dialog_update_file(job->progress, fpct, fbuf);

        IsdeProgress *p = job->progress;
        if (p && p->shell && p->graph) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (now.tv_sec - p->start_time.tv_sec) +
                             (now.tv_nsec - p->start_time.tv_nsec) / 1e9;
            long long total_done = atomic_load(&job->total_bytes_done);
            double dt = elapsed - p->prev_time;

            if (p->prev_time < 0.0) {
                p->prev_bytes = total_done;
                p->prev_time = elapsed;
            } else if (dt >= 0.1) {
                double rate_mbs = (total_done - p->prev_bytes)
                                  / (dt * 1024.0 * 1024.0);
                if (rate_mbs < 0.0) {
                    rate_mbs = 0.0;
                }
                p->prev_bytes = total_done;
                p->prev_time = elapsed;

                if (p->smooth_rate == 0.0) {
                    p->smooth_rate = rate_mbs;
                } else {
                    p->smooth_rate += 0.3 * (rate_mbs - p->smooth_rate);
                }

                kdata_vector_append(p->graph_data, elapsed, p->smooth_rate);
                IswGraphRedraw(p->graph);

                char rbuf[64];
                if (p->smooth_rate >= 1024.0) {
                    snprintf(rbuf, sizeof(rbuf), "%.1f GB/s", p->smooth_rate / 1024.0);
                } else if (p->smooth_rate >= 1.0) {
                    snprintf(rbuf, sizeof(rbuf), "%.1f MB/s", p->smooth_rate);
                } else {
                    snprintf(rbuf, sizeof(rbuf), "%.0f KB/s", p->smooth_rate * 1024.0);
                }
                if (p->rate_label &&
                    strcmp(rbuf, p->last_rate_msg) != 0) {
                    IswArgBuilder rab = IswArgBuilderInit();
                    IswArgLabel(&rab, rbuf);
                    IswSetValues(p->rate_label, rab.args, rab.count);
                    snprintf(p->last_rate_msg, sizeof(p->last_rate_msg),
                             "%s", rbuf);
                }
            }
        }
    }

    Fm *win = job->origin_win;
    if (win) {
        job->progress_timer = IswAppAddTimeOut(
            win->app_state->app, POLL_INTERVAL_MS, poll_timer_cb, job);
    }
}

void progress_start(FmApp *app, FmJob *job)
{
    Fm *win = job->origin_win;
    if (!win) return;

    job->progress = progress_dialog_create(win->toplevel,
                                           job_type_verb(job->type),
                                           app->app, cancel_cb, job);

    job->progress_timer = IswAppAddTimeOut(
        app->app, POLL_INTERVAL_MS, poll_timer_cb, job);
}

void progress_stop(FmJob *job)
{
    if (job->progress_timer) {
        IswRemoveTimeOut(job->progress_timer);
        job->progress_timer = 0;
    }
    progress_dialog_destroy(job->progress);
    job->progress = NULL;
}
