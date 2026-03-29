#define _POSIX_C_SOURCE 200809L
/*
 * fm.c — app init, menu setup, navigation logic
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------- forward declarations ---------- */

static void fm_delete_selected(Fm *fm);

/* ---------- rename dialog ---------- */

static Widget rename_shell = NULL;
static Fm    *rename_fm = NULL;
static int    rename_index = -1;

static void rename_ok_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Widget dialog = (Widget)cd;
    char *newname = IswDialogGetValueString(dialog);
    if (newname && newname[0] && rename_fm && rename_index >= 0 &&
        rename_index < rename_fm->nentries) {
        fileops_rename(rename_fm, rename_fm->entries[rename_index].full_path,
                       newname);
        fm_refresh(rename_fm);
    }
    if (rename_shell) {
        XtPopdown(rename_shell);
        XtDestroyWidget(rename_shell);
        rename_shell = NULL;
    }
}

static void rename_cancel_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)cd; (void)call;
    if (rename_shell) {
        XtPopdown(rename_shell);
        XtDestroyWidget(rename_shell);
        rename_shell = NULL;
    }
}

static void act_dismiss_rename(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)w; (void)ev; (void)p; (void)n;
    if (rename_shell) {
        XtPopdown(rename_shell);
        XtDestroyWidget(rename_shell);
        rename_shell = NULL;
    }
}

void show_rename_dialog(Fm *fm)
{
    /* Get selected index */
    int sel = -1;
    if (fm->iconview)
        sel = IswIconViewGetSelected(fm->iconview);
    if (sel < 0 || sel >= fm->nentries)
        return;

    rename_fm = fm;
    rename_index = sel;

    /* Destroy previous dialog if open */
    if (rename_shell) {
        XtDestroyWidget(rename_shell);
        rename_shell = NULL;
    }

    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNwidth, 300);              n++;
    XtSetArg(args[n], XtNheight, 120);             n++;
    XtSetArg(args[n], XtNborderWidth, 1);          n++;
    rename_shell = XtCreatePopupShell("renameShell",
                                      transientShellWidgetClass,
                                      fm->toplevel, args, n);
    XtOverrideTranslations(rename_shell, XtParseTranslationTable(
        "<Message>WM_PROTOCOLS: fm-dismiss-rename()\n"));

    n = 0;
    XtSetArg(args[n], XtNlabel, "Rename:");         n++;
    XtSetArg(args[n], XtNvalue, fm->entries[sel].name); n++;
    Widget dialog = XtCreateManagedWidget("renameDialog", dialogWidgetClass,
                                          rename_shell, args, n);

    IswDialogAddButton(dialog, "OK", rename_ok_cb, (XtPointer)dialog);
    IswDialogAddButton(dialog, "Cancel", rename_cancel_cb, NULL);

    XtPopup(rename_shell, XtGrabNone);
}

/* ---------- delete with confirmation ---------- */

static Widget delete_shell = NULL;
static Fm   *delete_fm = NULL;

static void dismiss_delete_dialog(void)
{
    if (delete_shell) {
        XtPopdown(delete_shell);
        XtDestroyWidget(delete_shell);
        delete_shell = NULL;
    }
}

/* WM_DELETE_WINDOW action for dialog shells — dismiss instead of exit */
static void act_dismiss_delete(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)w; (void)ev; (void)p; (void)n;
    dismiss_delete_dialog();
}

static void delete_do_trash(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)cd; (void)call;
    Fm *fm = delete_fm;
    if (!fm || !fm->iconview) { dismiss_delete_dialog(); return; }

    int *indices = NULL;
    int nsel = IswIconViewGetSelectedItems(fm->iconview, &indices);
    for (int i = nsel - 1; i >= 0; i--) {
        int idx = indices[i];
        if (idx >= 0 && idx < fm->nentries)
            fileops_trash(fm->entries[idx].full_path);
    }
    free(indices);
    if (nsel > 0) fm_refresh(fm);
    dismiss_delete_dialog();
}

static void delete_do_permanent(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)cd; (void)call;
    Fm *fm = delete_fm;
    if (!fm || !fm->iconview) { dismiss_delete_dialog(); return; }

    int *indices = NULL;
    int nsel = IswIconViewGetSelectedItems(fm->iconview, &indices);
    for (int i = nsel - 1; i >= 0; i--) {
        int idx = indices[i];
        if (idx >= 0 && idx < fm->nentries)
            fileops_delete(fm, fm->entries[idx].full_path);
    }
    free(indices);
    if (nsel > 0) fm_refresh(fm);
    dismiss_delete_dialog();
}

static void delete_confirm_cancel(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)cd; (void)call;
    dismiss_delete_dialog();
}

/* permanent = 0: trash mode (Delete key), permanent = 1: permanent mode (Shift+Delete) */
static void fm_delete_confirm(Fm *fm, int permanent)
{
    if (!fm->iconview) return;

    int *indices = NULL;
    int nsel = IswIconViewGetSelectedItems(fm->iconview, &indices);
    if (nsel <= 0) {
        free(indices);
        return;
    }

    char *trash_path = fileops_trash_path();
    int in_trash = (strncmp(fm->cwd, trash_path, strlen(trash_path)) == 0);
    free(trash_path);

    /* In trash or shift+delete: permanent mode */
    if (in_trash) permanent = 1;

    /* Build confirmation message */
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

    delete_fm = fm;

    if (delete_shell) {
        XtDestroyWidget(delete_shell);
        delete_shell = NULL;
    }

    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNwidth, isde_scale(300));  n++;
    XtSetArg(args[n], XtNheight, isde_scale(120)); n++;
    XtSetArg(args[n], XtNborderWidth, 1);           n++;
    delete_shell = XtCreatePopupShell("deleteShell",
                                       transientShellWidgetClass,
                                       fm->toplevel, args, n);
    XtOverrideTranslations(delete_shell, XtParseTranslationTable(
        "<Message>WM_PROTOCOLS: fm-dismiss-delete()\n"));

    n = 0;
    XtSetArg(args[n], XtNlabel, msg); n++;
    Widget dialog = XtCreateManagedWidget("deleteDialog", dialogWidgetClass,
                                           delete_shell, args, n);

    if (permanent)
        IswDialogAddButton(dialog, "Delete", delete_do_permanent, NULL);
    else
        IswDialogAddButton(dialog, "Move", delete_do_trash, NULL);
    IswDialogAddButton(dialog, "Cancel", delete_confirm_cancel, NULL);

    XtPopup(delete_shell, XtGrabNone);
}

static void fm_delete_selected(Fm *fm)
{
    fm_delete_confirm(fm, 0);
}

static void fm_delete_selected_permanent(Fm *fm)
{
    fm_delete_confirm(fm, 1);
}

/* ---------- empty trash ---------- */

static Widget empty_trash_shell = NULL;

static void empty_trash_ok(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
    fileops_empty_trash();
    char *trash_p = fileops_trash_path();
    if (strncmp(fm->cwd, trash_p, strlen(trash_p)) == 0)
        fm_refresh(fm);
    free(trash_p);
    if (empty_trash_shell) {
        XtPopdown(empty_trash_shell);
        XtDestroyWidget(empty_trash_shell);
        empty_trash_shell = NULL;
    }
}

static void empty_trash_cancel(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)cd; (void)call;
    if (empty_trash_shell) {
        XtPopdown(empty_trash_shell);
        XtDestroyWidget(empty_trash_shell);
        empty_trash_shell = NULL;
    }
}

static void act_dismiss_empty_trash(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)w; (void)ev; (void)p; (void)n;
    if (empty_trash_shell) {
        XtPopdown(empty_trash_shell);
        XtDestroyWidget(empty_trash_shell);
        empty_trash_shell = NULL;
    }
}

static void ctx_empty_trash(Fm *fm)
{
    if (empty_trash_shell) {
        XtDestroyWidget(empty_trash_shell);
        empty_trash_shell = NULL;
    }

    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNwidth, isde_scale(300));  n++;
    XtSetArg(args[n], XtNheight, isde_scale(120)); n++;
    XtSetArg(args[n], XtNborderWidth, 1);           n++;
    empty_trash_shell = XtCreatePopupShell("emptyTrashShell",
                                            transientShellWidgetClass,
                                            fm->toplevel, args, n);
    XtOverrideTranslations(empty_trash_shell, XtParseTranslationTable(
        "<Message>WM_PROTOCOLS: fm-dismiss-empty-trash()\n"));

    n = 0;
    XtSetArg(args[n], XtNlabel, "Permanently delete all items in Trash?"); n++;
    Widget dialog = XtCreateManagedWidget("emptyTrashDialog", dialogWidgetClass,
                                           empty_trash_shell, args, n);

    IswDialogAddButton(dialog, "Empty Trash", empty_trash_ok, fm);
    IswDialogAddButton(dialog, "Cancel", empty_trash_cancel, NULL);

    XtPopup(empty_trash_shell, XtGrabNone);
}

static void ctx_open_terminal(Fm *fm)
{
    pid_t pid = fork();
    if (pid == 0) {
        chdir(fm->cwd);
        execlp("xterm", "xterm", (char *)NULL);
        _exit(127);
    }
}

/* ---------- right-click context menu (List-based) ---------- */

static Widget  ctx_shell = NULL;
static Widget  ctx_list  = NULL;
static Fm     *ctx_fm    = NULL;

/* Menu items — parallel arrays: labels for display, callbacks for action */
typedef void (*CtxAction)(Fm *);

static void ctx_cut(Fm *fm)    { clipboard_cut(fm); }
static void ctx_copy(Fm *fm)   { clipboard_copy(fm); }
static void ctx_paste(Fm *fm)  { clipboard_paste(fm); }
static void ctx_rename(Fm *fm) { show_rename_dialog(fm); }
static void ctx_delete_action(Fm *fm) {
    fm_delete_selected(fm);
}
static void ctx_new_folder(Fm *fm) {
    fileops_mkdir(fm, "New Folder");
    fm_refresh(fm);
}
static void ctx_restore(Fm *fm) {
    if (!fm->iconview) return;
    int *indices = NULL;
    int nsel = IswIconViewGetSelectedItems(fm->iconview, &indices);
    for (int i = 0; i < nsel; i++) {
        int idx = indices[i];
        if (idx >= 0 && idx < fm->nentries)
            fileops_restore(fm->entries[idx].name);
    }
    free(indices);
    if (nsel > 0) fm_refresh(fm);
}

static String base_labels[] = {
    "Cut", "Copy", "Paste", "---",
    "Rename", "Delete", "---",
    "New Folder", "Open Terminal Here"
};
static CtxAction base_actions[] = {
    ctx_cut, ctx_copy, ctx_paste, NULL,
    ctx_rename, ctx_delete_action, NULL,
    ctx_new_folder, ctx_open_terminal
};
#define BASE_NITEMS 9

static String ctx_trash_labels[] = {
    "Restore", "Delete Permanently", "---", "Empty Trash", NULL
};
static CtxAction ctx_trash_actions[] = {
    ctx_restore, ctx_delete_action, NULL, ctx_empty_trash
};
#define CTX_TRASH_NITEMS 4

static int ctx_in_trash = 0;

/* Dynamic menu state — freed on dismiss */
static String    *dyn_labels;
static CtxAction *dyn_actions;
static int        dyn_nitems;

/* "Open with" app entries — desktop entry indices into fm->desktop_entries */
#define MAX_OPEN_WITH 16
static int   ow_indices[MAX_OPEN_WITH];  /* desktop_entries[] index */
static int   ow_count;
static char *ow_label_buf[MAX_OPEN_WITH]; /* "Open with AppName" strings */
static char *ow_file_path;               /* path of file to open */

static void ctx_open_with(Fm *fm)
{
    /* Determine which "Open with" entry was selected — the callback index
     * is embedded in dyn_actions; we find it via a range of stub functions.
     * Instead, we use a single action and store the app index separately. */
}

static void ctx_free_dynamic(void)
{
    free(dyn_labels);
    dyn_labels = NULL;
    free(dyn_actions);
    dyn_actions = NULL;
    dyn_nitems = 0;
    for (int i = 0; i < ow_count; i++)
        free(ow_label_buf[i]);
    ow_count = 0;
    free(ow_file_path);
    ow_file_path = NULL;
}

void fm_dismiss_context(void)
{
    if (ctx_shell) {
        XtPopdown(ctx_shell);
        XtDestroyWidget(ctx_shell);
        ctx_shell = NULL;
        ctx_list = NULL;
    }
    ctx_free_dynamic();
}

static void ctx_launch_open_with(Fm *fm, int ow_idx)
{
    if (ow_idx < 0 || ow_idx >= ow_count) return;
    IsdeDesktopEntry *de = fm->desktop_entries[ow_indices[ow_idx]];
    const char *file = ow_file_path;
    if (!file) return;

    char *cmd = isde_desktop_build_exec(de, &file, 1);
    if (!cmd) return;

    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    free(cmd);
}

static void ctx_select_cb(Widget w, XtPointer client_data,
                          XtPointer call_data)
{
    (void)w;
    (void)client_data;
    IswListReturnStruct *ret = (IswListReturnStruct *)call_data;
    int idx = ret->list_index;

    /* Save what we need before dismiss destroys the dynamic state */
    Fm *fm = ctx_fm;
    int in_trash = ctx_in_trash;

    if (!fm || idx < 0)  {
        fm_dismiss_context();
        return;
    }

    if (in_trash) {
        fm_dismiss_context();
        if (idx < CTX_TRASH_NITEMS && ctx_trash_actions[idx])
            ctx_trash_actions[idx](fm);
        return;
    }

    /* Check if this is an "Open with" entry */
    if (idx < ow_count) {
        /* Save values before dismiss frees them */
        int ow_idx = idx;
        int de_idx = ow_indices[ow_idx];
        IsdeDesktopEntry *de = fm->desktop_entries[de_idx];
        char *file = ow_file_path ? strdup(ow_file_path) : NULL;

        fm_dismiss_context();

        if (file && de) {
            char *cmd = isde_desktop_build_exec(de, (const char **)&file, 1);
            if (cmd) {
                pid_t pid = fork();
                if (pid == 0) {
                    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
                    _exit(127);
                }
                free(cmd);
            }
            free(file);
        }
        return;
    }

    /* Adjust index past "Open with" entries + separator */
    int base_idx = idx - (ow_count > 0 ? ow_count + 1 : 0);
    fm_dismiss_context();

    if (base_idx >= 0 && base_idx < BASE_NITEMS) {
        CtxAction action = base_actions[base_idx];
        if (action)
            action(fm);
    }
}

/* Build "Open with" entries for the selected file */
static void ctx_build_open_with(Fm *fm)
{
    ow_count = 0;

    if (!fm->iconview) return;
    int sel = IswIconViewGetSelected(fm->iconview);
    if (sel < 0 || sel >= fm->nentries) return;

    FmEntry *e = &fm->entries[sel];
    if (e->is_dir) return;

    const char *mime = isde_mime_type_for_file(e->name);
    if (!mime || strcmp(mime, "application/octet-stream") == 0) return;

    free(ow_file_path);
    ow_file_path = strdup(e->full_path);

    for (int i = 0; i < fm->ndesktop && ow_count < MAX_OPEN_WITH; i++) {
        IsdeDesktopEntry *de = fm->desktop_entries[i];
        if (!de) continue;
        if (isde_desktop_hidden(de) || isde_desktop_no_display(de)) continue;
        if (!isde_desktop_handles_mime(de, mime)) continue;
        const char *name = isde_desktop_name(de);
        if (!name) continue;

        ow_indices[ow_count] = i;
        char buf[256];
        snprintf(buf, sizeof(buf), "Open with %s", name);
        ow_label_buf[ow_count] = strdup(buf);
        ow_count++;
    }
}

/* Build the dynamic labels/actions arrays */
static void ctx_build_menu(Fm *fm)
{
    int total = (ow_count > 0 ? ow_count + 1 : 0) + BASE_NITEMS;

    dyn_labels = malloc((total + 1) * sizeof(String));
    dyn_actions = malloc(total * sizeof(CtxAction));
    dyn_nitems = total;

    int pos = 0;

    /* "Open with" entries */
    for (int i = 0; i < ow_count; i++) {
        dyn_labels[pos] = ow_label_buf[i];
        dyn_actions[pos] = NULL; /* handled specially in ctx_select_cb */
        pos++;
    }
    if (ow_count > 0) {
        dyn_labels[pos] = "---";
        dyn_actions[pos] = NULL;
        pos++;
    }

    /* Base items */
    for (int i = 0; i < BASE_NITEMS; i++) {
        dyn_labels[pos] = base_labels[i];
        dyn_actions[pos] = base_actions[i];
        pos++;
    }
    dyn_labels[pos] = NULL;
}

static void ctx_handler(Widget w, XtPointer client_data,
                        xcb_generic_event_t *event, Boolean *cont)
{
    (void)cont;
    if ((event->response_type & ~0x80) != XCB_BUTTON_PRESS)
        return;
    xcb_button_press_event_t *ev = (xcb_button_press_event_t *)event;
    if (ev->detail != 3)
        return;

    Fm *fm = (Fm *)client_data;
    ctx_fm = fm;

    /* Dismiss any existing context menu */
    fm_dismiss_context();

    /* Build "Open with" entries and dynamic menu */
    ctx_build_open_with(fm);
    ctx_build_menu(fm);

    /* Compute position at click */
    Position rx, ry;
    XtTranslateCoords(w, ev->event_x, ev->event_y, &rx, &ry);

    /* Create popup shell */
    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNx, rx);                   n++;
    XtSetArg(args[n], XtNy, ry);                   n++;
    XtSetArg(args[n], XtNoverrideRedirect, True);   n++;
    XtSetArg(args[n], XtNborderWidth, 1);           n++;
    ctx_shell = XtCreatePopupShell("ctxMenu", overrideShellWidgetClass,
                                   fm->toplevel, args, n);

    /* List of menu items */
    n = 0;
    XtSetArg(args[n], XtNlist, dyn_labels);          n++;
    XtSetArg(args[n], XtNnumberStrings, dyn_nitems); n++;
    XtSetArg(args[n], XtNdefaultColumns, 1);          n++;
    XtSetArg(args[n], XtNforceColumns, True);         n++;
    XtSetArg(args[n], XtNverticalList, True);         n++;
    XtSetArg(args[n], XtNborderWidth, 0);             n++;
    XtSetArg(args[n], XtNcursor, None);               n++;
    ctx_list = XtCreateManagedWidget("ctxList", listWidgetClass,
                                     ctx_shell, args, n);
    XtAddCallback(ctx_list, XtNcallback, ctx_select_cb, NULL);

    /* Hover-to-highlight, click-to-select */
    static char ctxTranslations[] =
        "<EnterWindow>: Set()\n"
        "<LeaveWindow>: Unset()\n"
        "<Motion>:      Set()\n"
        "<BtnDown>:     Set() Notify()\n"
        "<BtnUp>:       Notify()";
    XtOverrideTranslations(ctx_list,
                           XtParseTranslationTable(ctxTranslations));

    XtPopup(ctx_shell, XtGrabNone);
}

void fm_register_context_menu(Fm *fm, Widget w)
{
    XtAddEventHandler(w, ButtonPressMask, False, ctx_handler, fm);
}

/* ---------- navigation ---------- */

void fm_navigate(Fm *fm, const char *path)
{
    fm_dismiss_context();

    char *new_path = strdup(path);

    /* Clear view before freeing old entries */
    if (fm->iconview)
        IswIconViewSetItems(fm->iconview, NULL, NULL, 0);

    if (browser_read_dir(fm, new_path) != 0) {
        free(new_path);
        return;
    }

    free(fm->cwd);
    fm->cwd = new_path;

    /* Push to history, discarding forward entries */
    fm->hist_pos++;
    if (fm->hist_pos < FM_HISTORY_MAX) {
        for (int i = fm->hist_pos; i < fm->hist_count; i++)
            free(fm->history[i]);
        fm->history[fm->hist_pos] = strdup(new_path);
        fm->hist_count = fm->hist_pos + 1;
    }

    fileview_populate(fm);
    navbar_update(fm);
}

void fm_refresh(Fm *fm)
{
    fm_dismiss_context();

    /* Clear the view before freeing entries — the IconView holds
     * pointers to the old label/icon strings */
    if (fm->iconview)
        IswIconViewSetItems(fm->iconview, NULL, NULL, 0);

    browser_read_dir(fm, fm->cwd);
    fileview_populate(fm);
    navbar_update(fm);
}

/* ---------- keyboard shortcuts ---------- */

static Fm *shortcut_fm = NULL;  /* single instance for Xt actions */

static void act_copy(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)w; (void)ev; (void)p; (void)n;
    if (shortcut_fm) clipboard_copy(shortcut_fm);
}

static void act_cut(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)w; (void)ev; (void)p; (void)n;
    if (shortcut_fm) clipboard_cut(shortcut_fm);
}

static void act_paste(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)w; (void)ev; (void)p; (void)n;
    if (shortcut_fm) clipboard_paste(shortcut_fm);
}

static void act_delete(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)w; (void)ev; (void)p; (void)n;
    if (shortcut_fm) fm_delete_selected(shortcut_fm);
}

static void act_delete_permanent(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)w; (void)ev; (void)p; (void)n;
    if (shortcut_fm) fm_delete_selected_permanent(shortcut_fm);
}

static void act_rename(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)w; (void)ev; (void)p; (void)n;
    if (shortcut_fm) show_rename_dialog(shortcut_fm);
}

static void act_go_up(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)w; (void)ev; (void)p; (void)n;
    Fm *fm = shortcut_fm;
    if (!fm) return;
    char *parent = strdup(fm->cwd);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent)
        *slash = '\0';
    else {
        free(parent);
        parent = strdup("/");
    }
    fm_navigate(fm, parent);
    free(parent);
}

static void act_go_back(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)w; (void)ev; (void)p; (void)n;
    Fm *fm = shortcut_fm;
    if (!fm || fm->hist_pos <= 0) return;
    fm->hist_pos--;
    free(fm->cwd);
    fm->cwd = strdup(fm->history[fm->hist_pos]);
    fm_refresh(fm);
}

static void act_go_fwd(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)w; (void)ev; (void)p; (void)n;
    Fm *fm = shortcut_fm;
    if (!fm || fm->hist_pos >= fm->hist_count - 1) return;
    fm->hist_pos++;
    free(fm->cwd);
    fm->cwd = strdup(fm->history[fm->hist_pos]);
    fm_refresh(fm);
}

static void act_refresh(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)w; (void)ev; (void)p; (void)n;
    if (shortcut_fm) fm_refresh(shortcut_fm);
}

static void act_open(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)w; (void)ev; (void)p; (void)n;
    Fm *fm = shortcut_fm;
    if (!fm || !fm->iconview) return;
    int sel = IswIconViewGetSelected(fm->iconview);
    if (sel >= 0 && sel < fm->nentries)
        browser_open_entry(fm, sel);
}

static void act_toggle_hidden(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)w; (void)ev; (void)p; (void)n;
    Fm *fm = shortcut_fm;
    if (!fm) return;
    fm->show_hidden = !fm->show_hidden;
    fm_refresh(fm);
}

static XtActionsRec fm_actions[] = {
    {"fm-copy",          act_copy},
    {"fm-cut",           act_cut},
    {"fm-paste",         act_paste},
    {"fm-delete",        act_delete},
    {"fm-delete-permanent", act_delete_permanent},
    {"fm-rename",        act_rename},
    {"fm-go-up",         act_go_up},
    {"fm-go-back",       act_go_back},
    {"fm-go-fwd",        act_go_fwd},
    {"fm-refresh",       act_refresh},
    {"fm-open",          act_open},
    {"fm-toggle-hidden", act_toggle_hidden},
    {"fm-dismiss-rename", act_dismiss_rename},
    {"fm-dismiss-delete", act_dismiss_delete},
    {"fm-dismiss-empty-trash", act_dismiss_empty_trash},
};

static char fm_translations[] =
    "Ctrl<Key>c:        fm-copy()\n"
    "Ctrl<Key>x:        fm-cut()\n"
    "Ctrl<Key>v:        fm-paste()\n"
    "Shift<Key>Delete:    fm-delete-permanent()\n"
    "Shift<Key>KP_Delete: fm-delete-permanent()\n"
    "<Key>Delete:        fm-delete()\n"
    "<Key>KP_Delete:     fm-delete()\n"
    "<Key>F2:            fm-rename()\n"
    "<Key>BackSpace:     fm-go-up()\n"
    "Alt<Key>Left:       fm-go-back()\n"
    "Alt<Key>Right:      fm-go-fwd()\n"
    "<Key>F5:            fm-refresh()\n"
    "Ctrl<Key>h:        fm-toggle-hidden()\n";

void fm_install_shortcuts(Widget w)
{
    XtOverrideTranslations(w, XtParseTranslationTable(fm_translations));
}

/* ---------- D-Bus settings reload ---------- */

static void fm_reload_config(Fm *fm)
{
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("fm.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *gen = isde_config_table(root, "general");
        if (gen) {
            const char *click = isde_config_string(gen, "click_to_open", "double");
            fm->double_click = (strcmp(click, "single") != 0);
            fm->show_hidden = isde_config_bool(gen, "show_hidden", 0);
        }
        isde_config_free(cfg);
    }
    isde_config_invalidate_cache();
    fm_refresh(fm);
}

static void on_settings_changed(const char *section, const char *key,
                                void *user_data)
{
    (void)key;
    Fm *fm = (Fm *)user_data;
    if (strcmp(section, "general") == 0 ||
        strcmp(section, "input") == 0 ||
        strcmp(section, "appearance") == 0 ||
        strcmp(section, "*") == 0) {
        if (strcmp(section, "appearance") == 0 || strcmp(section, "*") == 0) {
            isde_theme_reload();
            icons_init(); /* re-scan with new theme */
        }
        fm_reload_config(fm);
    }
}

static void dbus_input_cb(XtPointer client_data, int *fd, XtInputId *id)
{
    (void)fd;
    (void)id;
    IsdeDBus *bus = (IsdeDBus *)client_data;
    isde_dbus_dispatch(bus);
}

/* ---------- close handling ---------- */

static void fm_destroy_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
    fm->running = 0;
    XtAppSetExitFlag(fm->app);
}

/* ---------- init ---------- */

int fm_init(Fm *fm, int *argc, char **argv)
{
    memset(fm, 0, sizeof(*fm));

    /* Load config */
    fm->double_click = 1; /* default */
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("fm.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *gen = isde_config_table(root, "general");
        if (gen) {
            const char *click = isde_config_string(gen, "click_to_open", "double");
            fm->double_click = (strcmp(click, "single") != 0);
            fm->show_hidden = isde_config_bool(gen, "show_hidden", 0);
        }
        isde_config_free(cfg);
    }

    char **fallbacks = isde_theme_build_resources();
    fm->toplevel = XtAppInitialize(&fm->app, "ISDE-FM",
                                   NULL, 0, argc, argv,
                                   fallbacks, NULL, 0);

    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNwidth, isde_scale(700));  n++;
    XtSetArg(args[n], XtNheight, isde_scale(500)); n++;
    XtSetValues(fm->toplevel, args, n);

    XtAddCallback(fm->toplevel, XtNdestroyCallback, fm_destroy_cb, fm);

    /* MainWindow — no menu bar, content only */
    fm->main_window = XtCreateManagedWidget("mainWin", mainWindowWidgetClass,
                                            fm->toplevel, NULL, 0);
    XtUnmanageChild(IswMainWindowGetMenuBar(fm->main_window));

    /* Outer FlexBox: vertical — navbar on top, body fills remainder */
    n = 0;
    XtSetArg(args[n], XtNorientation, XtorientVertical); n++;
    XtSetArg(args[n], XtNborderWidth, 0);                 n++;
    fm->vbox = XtCreateManagedWidget("vbox", flexBoxWidgetClass,
                                      fm->main_window, args, n);

    /* Navigation bar — no grow, gets its preferred height */
    navbar_init(fm);

    /* Content area: horizontal FlexBox (sidebar | fileview) */
    n = 0;
    XtSetArg(args[n], XtNorientation, XtorientHorizontal); n++;
    XtSetArg(args[n], XtNborderWidth, 0);                   n++;
    XtSetArg(args[n], XtNflexGrow, 1);                      n++;
    fm->hbox = XtCreateManagedWidget("hbox", flexBoxWidgetClass,
                                      fm->vbox, args, n);

    /* Places sidebar — fixed width, no grow */
    icons_init();
    places_init(fm);

    /* File view — grows to fill remaining width */
    fileview_init(fm);

    /* Keyboard shortcuts — register actions globally */
    shortcut_fm = fm;
    XtAppAddActions(fm->app, fm_actions, XtNumber(fm_actions));

    /* Clipboard init (atoms only — no XDND dependency) */
    clipboard_init(fm);

    /* D-Bus settings notifications */
    fm->dbus = isde_dbus_init();
    if (fm->dbus) {
        isde_dbus_settings_subscribe(fm->dbus, on_settings_changed, fm);
        int dbus_fd = isde_dbus_get_fd(fm->dbus);
        if (dbus_fd >= 0)
            XtAppAddInput(fm->app, dbus_fd,
                          (XtPointer)XtInputReadMask,
                          dbus_input_cb, fm->dbus);
    }

    /* Start in home directory */
    /* Cache desktop entries for "Open with" */
    {
        static const char *app_dirs[] = {
            "/usr/share/applications",
            "/usr/local/share/applications",
            NULL
        };
        /* Also check $HOME/.local/share/applications */
        const char *home_env = getenv("HOME");
        char local_apps[512] = "";
        if (home_env)
            snprintf(local_apps, sizeof(local_apps),
                     "%s/.local/share/applications", home_env);

        fm->desktop_entries = NULL;
        fm->ndesktop = 0;
        int cap = 0;
        for (int d = -1; app_dirs[d + 1] || d < 0; d++) {
            const char *dir = (d < 0) ? local_apps : app_dirs[d];
            if (!dir[0]) continue;
            int count = 0;
            IsdeDesktopEntry **batch = isde_desktop_scan_dir(dir, &count);
            if (!batch) continue;
            if (fm->ndesktop + count > cap) {
                cap = fm->ndesktop + count + 64;
                fm->desktop_entries = realloc(fm->desktop_entries,
                                              cap * sizeof(IsdeDesktopEntry *));
            }
            for (int i = 0; i < count; i++)
                fm->desktop_entries[fm->ndesktop++] = batch[i];
            free(batch);
        }
    }

    const char *home = getenv("HOME");
    fm->cwd = strdup(home ? home : "/");
    fm->history[0] = strdup(fm->cwd);
    fm->hist_pos = 0;
    fm->hist_count = 1;

    browser_read_dir(fm, fm->cwd);

    XtRealizeWidget(fm->toplevel);

    /* XDND init must be after realize — Shell.c calls ISWXdndEnable
     * during realize, so the XDND state exists only after this point. */
    dnd_init(fm);

    fileview_populate(fm);
    navbar_update(fm);

    fm->running = 1;
    return 0;
}

void fm_run(Fm *fm)
{
    while (fm->running && !XtAppGetExitFlag(fm->app))
        XtAppProcessEvent(fm->app, XtIMAll);
}

void fm_cleanup(Fm *fm)
{
    isde_dbus_free(fm->dbus);
    dnd_cleanup(fm);
    clipboard_cleanup(fm);
    browser_free_entries(fm);
    fileview_cleanup(fm);
    places_cleanup(fm);
    icons_cleanup();
    free(fm->cwd);
    for (int i = 0; i < fm->hist_count; i++)
        free(fm->history[i]);
    for (int i = 0; i < fm->ndesktop; i++)
        isde_desktop_free(fm->desktop_entries[i]);
    free(fm->desktop_entries);
    XtDestroyApplicationContext(fm->app);
}
