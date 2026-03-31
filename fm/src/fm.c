#define _POSIX_C_SOURCE 200809L
/*
 * fm.c — app init, menu setup, navigation logic
 */
#include "fm.h"

#include <stdio.h>
#include "isde/isde-ewmh.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* App-wide shared state (will move to separate allocation in phase 2) */
static FmApp g_app;

/* Context key for storing Fm* on shell windows */
XContext fm_window_context = 0;

/* ---------- forward declarations ---------- */

static void fm_delete_selected(Fm *fm);
static void fm_delete_selected_permanent(Fm *fm);
static void act_new_window(Widget, xcb_generic_event_t *, String *, Cardinal *);
static void act_close_window(Widget, xcb_generic_event_t *, String *, Cardinal *);
static void app_remove_window(FmApp *app, Fm *fm);
static void ctx_free_dynamic(Fm *fm);

/* ---------- rename dialog ---------- */

static void rename_ok_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Widget dialog = (Widget)cd;
    Fm *fm = fm_from_widget(dialog);
    if (!fm) {
        return;
    }
    char *newname = IswDialogGetValueString(dialog);
    if (newname && newname[0] && fm->rename_index >= 0 &&
        fm->rename_index < fm->nentries) {
        fileops_rename(fm, fm->entries[fm->rename_index].full_path,
                       newname);
        fm_refresh(fm);
    }
    if (fm->rename_shell) {
        XtPopdown(fm->rename_shell);
        XtDestroyWidget(fm->rename_shell);
        fm->rename_shell = NULL;
    }
}

static void rename_cancel_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)cd; (void)call;
    Fm *fm = fm_from_widget(w);
    if (fm && fm->rename_shell) {
        XtPopdown(fm->rename_shell);
        XtDestroyWidget(fm->rename_shell);
        fm->rename_shell = NULL;
    }
}

static void act_dismiss_rename(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm && fm->rename_shell) {
        XtPopdown(fm->rename_shell);
        XtDestroyWidget(fm->rename_shell);
        fm->rename_shell = NULL;
    }
}

void show_rename_dialog(Fm *fm)
{
    int sel = -1;
    if (fm->iconview) {
        sel = IswIconViewGetSelected(fm->iconview);
    }
    if (sel < 0 || sel >= fm->nentries) {
        return;
    }

    fm->rename_index = sel;

    if (fm->rename_shell) {
        XtDestroyWidget(fm->rename_shell);
        fm->rename_shell = NULL;
    }

    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNwidth, isde_scale(300));   n++;
    XtSetArg(args[n], XtNheight, isde_scale(120)); n++;
    XtSetArg(args[n], XtNborderWidth, 1);          n++;
    fm->rename_shell = XtCreatePopupShell("renameShell",
                                      transientShellWidgetClass,
                                      fm->toplevel, args, n);
    XtOverrideTranslations(fm->rename_shell, XtParseTranslationTable(
        "<Message>WM_PROTOCOLS: fm-dismiss-rename()\n"));

    n = 0;
    XtSetArg(args[n], XtNlabel, "Rename:");         n++;
    XtSetArg(args[n], XtNvalue, fm->entries[sel].name); n++;
    Widget dialog = XtCreateManagedWidget("renameDialog", dialogWidgetClass,
                                          fm->rename_shell, args, n);

    /* OK / Cancel buttons — HIG: action first, bottom-right */
    int btn_w = isde_scale(80);
    int btn_pad = isde_scale(8);
    Widget value_w = XtNameToWidget(dialog, "value");
    Widget anchor = value_w ? value_w : XtNameToWidget(dialog, "label");

    n = 0;
    XtSetArg(args[n], XtNlabel, "OK");                  n++;
    XtSetArg(args[n], XtNwidth, btn_w);                  n++;
    XtSetArg(args[n], XtNinternalWidth, btn_pad);        n++;
    XtSetArg(args[n], XtNinternalHeight, btn_pad);       n++;
    XtSetArg(args[n], XtNfromVert, anchor);              n++;
    XtSetArg(args[n], XtNhorizDistance, 300 - btn_w * 2 - btn_pad); n++;
    XtSetArg(args[n], XtNleft, XtChainRight);            n++;
    XtSetArg(args[n], XtNright, XtChainRight);           n++;
    XtSetArg(args[n], XtNbottom, XtChainBottom);         n++;
    XtSetArg(args[n], XtNtop, XtChainBottom);            n++;
    Widget ok = XtCreateManagedWidget("ok", commandWidgetClass,
                                      dialog, args, n);
    XtAddCallback(ok, XtNcallback, rename_ok_cb, (XtPointer)dialog);

    n = 0;
    XtSetArg(args[n], XtNlabel, "Cancel");               n++;
    XtSetArg(args[n], XtNwidth, btn_w);                  n++;
    XtSetArg(args[n], XtNinternalWidth, btn_pad);        n++;
    XtSetArg(args[n], XtNinternalHeight, btn_pad);       n++;
    XtSetArg(args[n], XtNfromVert, anchor);              n++;
    XtSetArg(args[n], XtNfromHoriz, ok);                 n++;
    XtSetArg(args[n], XtNhorizDistance, btn_pad);        n++;
    XtSetArg(args[n], XtNleft, XtChainRight);            n++;
    XtSetArg(args[n], XtNright, XtChainRight);           n++;
    XtSetArg(args[n], XtNbottom, XtChainBottom);         n++;
    XtSetArg(args[n], XtNtop, XtChainBottom);            n++;
    Widget cancel = XtCreateManagedWidget("cancel", commandWidgetClass,
                                          dialog, args, n);
    XtAddCallback(cancel, XtNcallback, rename_cancel_cb, NULL);

    XtPopup(fm->rename_shell, XtGrabExclusive);
}

/* ---------- delete with confirmation ---------- */

static void dismiss_delete_dialog(Fm *fm)
{
    if (fm->delete_shell) {
        XtPopdown(fm->delete_shell);
        XtDestroyWidget(fm->delete_shell);
        fm->delete_shell = NULL;
    }
}

static void act_dismiss_delete(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm) {
        dismiss_delete_dialog(fm);
    }
}

static void delete_do_trash(Widget w, XtPointer cd, XtPointer call)
{
    (void)cd; (void)call;
    Fm *fm = fm_from_widget(w);
    if (!fm || !fm->iconview) {
        if (fm) {
            dismiss_delete_dialog(fm);
        }
        return;
    }

    int *indices = NULL;
    int nsel = IswIconViewGetSelectedItems(fm->iconview, &indices);
    if (nsel > 0) {
        char **paths = malloc(nsel * sizeof(char *));
        int npaths = 0;
        for (int i = 0; i < nsel; i++) {
            int idx = indices[i];
            if (idx >= 0 && idx < fm->nentries) {
                paths[npaths++] = fm->entries[idx].full_path;
            }
        }
        if (npaths > 0) {
            jobqueue_submit_trash(fm->app_state, fm, paths, npaths);
        }
        free(paths);
    }
    free(indices);
    dismiss_delete_dialog(fm);
}

static void delete_do_permanent(Widget w, XtPointer cd, XtPointer call)
{
    (void)cd; (void)call;
    Fm *fm = fm_from_widget(w);
    if (!fm || !fm->iconview) {
        if (fm) {
            dismiss_delete_dialog(fm);
        }
        return;
    }

    int *indices = NULL;
    int nsel = IswIconViewGetSelectedItems(fm->iconview, &indices);
    if (nsel > 0) {
        char **paths = malloc(nsel * sizeof(char *));
        int npaths = 0;
        for (int i = 0; i < nsel; i++) {
            int idx = indices[i];
            if (idx >= 0 && idx < fm->nentries) {
                paths[npaths++] = fm->entries[idx].full_path;
            }
        }
        if (npaths > 0) {
            jobqueue_submit_delete(fm->app_state, fm, paths, npaths);
        }
        free(paths);
    }
    free(indices);
    dismiss_delete_dialog(fm);
}

static void delete_confirm_cancel(Widget w, XtPointer cd, XtPointer call)
{
    (void)cd; (void)call;
    Fm *fm = fm_from_widget(w);
    if (fm) {
        dismiss_delete_dialog(fm);
    }
}

static void fm_delete_confirm(Fm *fm, int permanent)
{
    if (!fm->iconview) {
        return;
    }

    int *indices = NULL;
    int nsel = IswIconViewGetSelectedItems(fm->iconview, &indices);
    if (nsel <= 0) {
        free(indices);
        return;
    }

    char *trash_path = fileops_trash_path();
    int in_trash = (strncmp(fm->cwd, trash_path, strlen(trash_path)) == 0);
    free(trash_path);

    if (in_trash) {
        permanent = 1;
    }

    char msg[256];
    if (nsel == 1) {
        int idx = indices[0];
        const char *name = (idx >= 0 && idx < fm->nentries)
                           ? fm->entries[idx].name : "selected item";
        if (permanent) {
            snprintf(msg, sizeof(msg), "Permanently delete \"%s\"?", name);
        } else {
            snprintf(msg, sizeof(msg), "Move \"%s\" to Trash?", name);
        }
    } else {
        if (permanent) {
            snprintf(msg, sizeof(msg), "Permanently delete %d items?", nsel);
        } else {
            snprintf(msg, sizeof(msg), "Move %d items to Trash?", nsel);
        }
    }
    free(indices);

    if (fm->delete_shell) {
        XtDestroyWidget(fm->delete_shell);
        fm->delete_shell = NULL;
    }

    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNwidth, isde_scale(300));   n++;
    XtSetArg(args[n], XtNheight, isde_scale(120));  n++;
    XtSetArg(args[n], XtNborderWidth, 1);           n++;
    fm->delete_shell = XtCreatePopupShell("deleteShell",
                                       transientShellWidgetClass,
                                       fm->toplevel, args, n);
    XtOverrideTranslations(fm->delete_shell, XtParseTranslationTable(
        "<Message>WM_PROTOCOLS: fm-dismiss-delete()\n"));

    n = 0;
    XtSetArg(args[n], XtNlabel, msg); n++;
    Widget dialog = XtCreateManagedWidget("deleteDialog", dialogWidgetClass,
                                           fm->delete_shell, args, n);

    /* Action / Cancel buttons — HIG: action first, bottom-right */
    int btn_w = isde_scale(80);
    int btn_pad = isde_scale(8);
    Widget del_anchor = XtNameToWidget(dialog, "label");

    n = 0;
    XtSetArg(args[n], XtNlabel, permanent ? "Delete" : "Move"); n++;
    XtSetArg(args[n], XtNwidth, btn_w);                  n++;
    XtSetArg(args[n], XtNinternalWidth, btn_pad);        n++;
    XtSetArg(args[n], XtNinternalHeight, btn_pad);       n++;
    XtSetArg(args[n], XtNfromVert, del_anchor);          n++;
    XtSetArg(args[n], XtNhorizDistance, isde_scale(300) - btn_w * 2 - btn_pad); n++;
    XtSetArg(args[n], XtNleft, XtChainRight);            n++;
    XtSetArg(args[n], XtNright, XtChainRight);           n++;
    XtSetArg(args[n], XtNbottom, XtChainBottom);         n++;
    XtSetArg(args[n], XtNtop, XtChainBottom);            n++;
    Widget action = XtCreateManagedWidget("action", commandWidgetClass,
                                          dialog, args, n);
    XtAddCallback(action, XtNcallback,
                  permanent ? delete_do_permanent : delete_do_trash, NULL);

    n = 0;
    XtSetArg(args[n], XtNlabel, "Cancel");               n++;
    XtSetArg(args[n], XtNwidth, btn_w);                  n++;
    XtSetArg(args[n], XtNinternalWidth, btn_pad);        n++;
    XtSetArg(args[n], XtNinternalHeight, btn_pad);       n++;
    XtSetArg(args[n], XtNfromVert, del_anchor);          n++;
    XtSetArg(args[n], XtNfromHoriz, action);             n++;
    XtSetArg(args[n], XtNhorizDistance, btn_pad);        n++;
    XtSetArg(args[n], XtNleft, XtChainRight);            n++;
    XtSetArg(args[n], XtNright, XtChainRight);           n++;
    XtSetArg(args[n], XtNbottom, XtChainBottom);         n++;
    XtSetArg(args[n], XtNtop, XtChainBottom);            n++;
    Widget cancel = XtCreateManagedWidget("cancel", commandWidgetClass,
                                          dialog, args, n);
    XtAddCallback(cancel, XtNcallback, delete_confirm_cancel, NULL);

    XtPopup(fm->delete_shell, XtGrabExclusive);
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

static void empty_trash_ok(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
    jobqueue_submit_empty_trash(fm->app_state, fm);
    if (fm->empty_trash_shell) {
        XtPopdown(fm->empty_trash_shell);
        XtDestroyWidget(fm->empty_trash_shell);
        fm->empty_trash_shell = NULL;
    }
}

static void empty_trash_cancel(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)cd; (void)call;
    Fm *fm = fm_from_widget(w);
    if (fm && fm->empty_trash_shell) {
        XtPopdown(fm->empty_trash_shell);
        XtDestroyWidget(fm->empty_trash_shell);
        fm->empty_trash_shell = NULL;
    }
}

static void act_dismiss_empty_trash(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm && fm->empty_trash_shell) {
        XtPopdown(fm->empty_trash_shell);
        XtDestroyWidget(fm->empty_trash_shell);
        fm->empty_trash_shell = NULL;
    }
}

static void ctx_empty_trash(Fm *fm)
{
    if (fm->empty_trash_shell) {
        XtDestroyWidget(fm->empty_trash_shell);
        fm->empty_trash_shell = NULL;
    }

    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNwidth, isde_scale(300));   n++;
    XtSetArg(args[n], XtNheight, isde_scale(120));  n++;
    XtSetArg(args[n], XtNborderWidth, 1);           n++;
    fm->empty_trash_shell = XtCreatePopupShell("emptyTrashShell",
                                            transientShellWidgetClass,
                                            fm->toplevel, args, n);
    XtOverrideTranslations(fm->empty_trash_shell, XtParseTranslationTable(
        "<Message>WM_PROTOCOLS: fm-dismiss-empty-trash()\n"));

    n = 0;
    XtSetArg(args[n], XtNlabel, "Permanently delete all items in Trash?"); n++;
    Widget dialog = XtCreateManagedWidget("emptyTrashDialog", dialogWidgetClass,
                                           fm->empty_trash_shell, args, n);

    /* Action / Cancel buttons — HIG: action first, bottom-right */
    int btn_w = isde_scale(80);
    int btn_pad = isde_scale(8);
    Widget et_anchor = XtNameToWidget(dialog, "label");

    n = 0;
    XtSetArg(args[n], XtNlabel, "Empty Trash");          n++;
    XtSetArg(args[n], XtNwidth, btn_w);                  n++;
    XtSetArg(args[n], XtNinternalWidth, btn_pad);        n++;
    XtSetArg(args[n], XtNinternalHeight, btn_pad);       n++;
    XtSetArg(args[n], XtNfromVert, et_anchor);           n++;
    XtSetArg(args[n], XtNhorizDistance, isde_scale(300) - btn_w * 2 - btn_pad); n++;
    XtSetArg(args[n], XtNleft, XtChainRight);            n++;
    XtSetArg(args[n], XtNright, XtChainRight);           n++;
    XtSetArg(args[n], XtNbottom, XtChainBottom);         n++;
    XtSetArg(args[n], XtNtop, XtChainBottom);            n++;
    Widget et_action = XtCreateManagedWidget("action", commandWidgetClass,
                                              dialog, args, n);
    XtAddCallback(et_action, XtNcallback, empty_trash_ok, fm);

    n = 0;
    XtSetArg(args[n], XtNlabel, "Cancel");               n++;
    XtSetArg(args[n], XtNwidth, btn_w);                  n++;
    XtSetArg(args[n], XtNinternalWidth, btn_pad);        n++;
    XtSetArg(args[n], XtNinternalHeight, btn_pad);       n++;
    XtSetArg(args[n], XtNfromVert, et_anchor);           n++;
    XtSetArg(args[n], XtNfromHoriz, et_action);          n++;
    XtSetArg(args[n], XtNhorizDistance, btn_pad);        n++;
    XtSetArg(args[n], XtNleft, XtChainRight);            n++;
    XtSetArg(args[n], XtNright, XtChainRight);           n++;
    XtSetArg(args[n], XtNbottom, XtChainBottom);         n++;
    XtSetArg(args[n], XtNtop, XtChainBottom);            n++;
    Widget et_cancel = XtCreateManagedWidget("cancel", commandWidgetClass,
                                              dialog, args, n);
    XtAddCallback(et_cancel, XtNcallback, empty_trash_cancel, NULL);

    XtPopup(fm->empty_trash_shell, XtGrabExclusive);
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
    if (!fm->iconview) {
        return;
    }
    int *indices = NULL;
    int nsel = IswIconViewGetSelectedItems(fm->iconview, &indices);
    for (int i = 0; i < nsel; i++) {
        int idx = indices[i];
        if (idx >= 0 && idx < fm->nentries) {
            fileops_restore(fm->entries[idx].name);
        }
    }
    free(indices);
    if (nsel > 0) {
        fm_refresh(fm);
    }
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

static void ctx_free_dynamic(Fm *fm)
{
    free(fm->dyn_labels);
    fm->dyn_labels = NULL;
    free(fm->dyn_actions);
    fm->dyn_actions = NULL;
    fm->dyn_nitems = 0;
    for (int i = 0; i < fm->ow_count; i++) {
        free(fm->ow_label_buf[i]);
    }
    fm->ow_count = 0;
    free(fm->ow_file_path);
    fm->ow_file_path = NULL;
}

void fm_dismiss_context(Fm *fm)
{
    if (fm->ctx_shell) {
        XtPopdown(fm->ctx_shell);
        XtDestroyWidget(fm->ctx_shell);
        fm->ctx_shell = NULL;
        fm->ctx_list = NULL;
    }
    ctx_free_dynamic(fm);
}

static void ctx_select_cb(Widget w, XtPointer client_data,
                          XtPointer call_data)
{
    (void)w;
    (void)client_data;
    IswListReturnStruct *ret = (IswListReturnStruct *)call_data;
    int idx = ret->list_index;

    /* Recover Fm from context menu's shell */
    Fm *fm = fm_from_widget(w);
    if (!fm) {
        return;
    }
    int in_trash = fm->ctx_in_trash;

    if (idx < 0) {
        fm_dismiss_context(fm);
        return;
    }

    if (in_trash) {
        fm_dismiss_context(fm);
        if (idx < CTX_TRASH_NITEMS && ctx_trash_actions[idx]) {
            ctx_trash_actions[idx](fm);
        }
        return;
    }

    /* Check if this is an "Open with" entry */
    if (idx < fm->ow_count) {
        int de_idx = fm->ow_indices[idx];
        FmApp *app = fm->app_state;
        IsdeDesktopEntry *de = app->desktop_entries[de_idx];
        char *file = fm->ow_file_path ? strdup(fm->ow_file_path) : NULL;

        fm_dismiss_context(fm);

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
    int base_idx = idx - (fm->ow_count > 0 ? fm->ow_count + 1 : 0);
    fm_dismiss_context(fm);

    if (base_idx >= 0 && base_idx < BASE_NITEMS) {
        CtxAction action = base_actions[base_idx];
        if (action) {
            action(fm);
        }
    }
}

static void ctx_build_open_with(Fm *fm)
{
    fm->ow_count = 0;
    FmApp *app = fm->app_state;

    if (!fm->iconview) {
        return;
    }
    int sel = IswIconViewGetSelected(fm->iconview);
    if (sel < 0 || sel >= fm->nentries) {
        return;
    }

    FmEntry *e = &fm->entries[sel];
    if (e->is_dir) {
        return;
    }

    const char *mime = isde_mime_type_for_file(e->name);
    if (!mime || strcmp(mime, "application/octet-stream") == 0) {
        return;
    }

    free(fm->ow_file_path);
    fm->ow_file_path = strdup(e->full_path);

    for (int i = 0; i < app->ndesktop && fm->ow_count < MAX_OPEN_WITH; i++) {
        IsdeDesktopEntry *de = app->desktop_entries[i];
        if (!de) {
            continue;
        }
        if (isde_desktop_hidden(de) || isde_desktop_no_display(de)) {
            continue;
        }
        if (!isde_desktop_handles_mime(de, mime)) {
            continue;
        }
        const char *name = isde_desktop_name(de);
        if (!name) {
            continue;
        }

        fm->ow_indices[fm->ow_count] = i;
        char buf[256];
        snprintf(buf, sizeof(buf), "Open with %s", name);
        fm->ow_label_buf[fm->ow_count] = strdup(buf);
        fm->ow_count++;
    }
}

static void ctx_build_menu(Fm *fm)
{
    int total = (fm->ow_count > 0 ? fm->ow_count + 1 : 0) + BASE_NITEMS;

    fm->dyn_labels = malloc((total + 1) * sizeof(String));
    fm->dyn_actions = malloc(total * sizeof(CtxAction));
    fm->dyn_nitems = total;

    int pos = 0;

    for (int i = 0; i < fm->ow_count; i++) {
        fm->dyn_labels[pos] = fm->ow_label_buf[i];
        fm->dyn_actions[pos] = NULL;
        pos++;
    }
    if (fm->ow_count > 0) {
        fm->dyn_labels[pos] = "---";
        fm->dyn_actions[pos] = NULL;
        pos++;
    }

    for (int i = 0; i < BASE_NITEMS; i++) {
        fm->dyn_labels[pos] = base_labels[i];
        fm->dyn_actions[pos] = base_actions[i];
        pos++;
    }
    fm->dyn_labels[pos] = NULL;
}

static void ctx_handler(Widget w, XtPointer client_data,
                        xcb_generic_event_t *event, Boolean *cont)
{
    (void)cont;
    if ((event->response_type & ~0x80) != XCB_BUTTON_PRESS) {
        return;
    }
    xcb_button_press_event_t *ev = (xcb_button_press_event_t *)event;
    if (ev->detail != 3) {
        return;
    }

    Fm *fm = (Fm *)client_data;

    fm_dismiss_context(fm);

    /* Check if we're in trash */
    char *trash_path = fileops_trash_path();
    fm->ctx_in_trash = (strncmp(fm->cwd, trash_path, strlen(trash_path)) == 0);
    free(trash_path);

    if (fm->ctx_in_trash) {
        /* Trash context menu */
        fm->dyn_labels = NULL;
        fm->dyn_actions = NULL;
        fm->dyn_nitems = CTX_TRASH_NITEMS;
    } else {
        ctx_build_open_with(fm);
        ctx_build_menu(fm);
    }

    Position rx, ry;
    XtTranslateCoords(w, ev->event_x, ev->event_y, &rx, &ry);

    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNx, rx);                    n++;
    XtSetArg(args[n], XtNy, ry);                    n++;
    XtSetArg(args[n], XtNoverrideRedirect, True);    n++;
    XtSetArg(args[n], XtNborderWidth, 1);            n++;
    fm->ctx_shell = XtCreatePopupShell("ctxMenu", overrideShellWidgetClass,
                                   fm->toplevel, args, n);

    String *labels = fm->ctx_in_trash ? ctx_trash_labels : fm->dyn_labels;
    int nitems = fm->ctx_in_trash ? CTX_TRASH_NITEMS : fm->dyn_nitems;

    n = 0;
    XtSetArg(args[n], XtNlist, labels);              n++;
    XtSetArg(args[n], XtNnumberStrings, nitems);     n++;
    XtSetArg(args[n], XtNdefaultColumns, 1);         n++;
    XtSetArg(args[n], XtNforceColumns, True);        n++;
    XtSetArg(args[n], XtNverticalList, True);        n++;
    XtSetArg(args[n], XtNborderWidth, 0);            n++;
    XtSetArg(args[n], XtNcursor, None);              n++;
    fm->ctx_list = XtCreateManagedWidget("ctxList", listWidgetClass,
                                     fm->ctx_shell, args, n);
    XtAddCallback(fm->ctx_list, XtNcallback, ctx_select_cb, NULL);

    static char ctxTranslations[] =
        "<EnterWindow>: Set()\n"
        "<LeaveWindow>: Unset()\n"
        "<Motion>:      Set()\n"
        "<BtnDown>:     Set() Notify()\n"
        "<BtnUp>:       Notify()";
    XtOverrideTranslations(fm->ctx_list,
                           XtParseTranslationTable(ctxTranslations));

    XtPopup(fm->ctx_shell, XtGrabNone);
}

void fm_register_context_menu(Fm *fm, Widget w)
{
    XtAddEventHandler(w, ButtonPressMask, False, ctx_handler, fm);
}

/* ---------- navigation ---------- */

void fm_navigate(Fm *fm, const char *path)
{
    fm_dismiss_context(fm);

    char *new_path = strdup(path);

    if (fm->iconview) {
        IswIconViewSetItems(fm->iconview, NULL, NULL, 0);
    }

    if (browser_read_dir(fm, new_path) != 0) {
        free(new_path);
        return;
    }

    free(fm->cwd);
    fm->cwd = new_path;

    fm->hist_pos++;
    if (fm->hist_pos < FM_HISTORY_MAX) {
        for (int i = fm->hist_pos; i < fm->hist_count; i++) {
            free(fm->history[i]);
        }
        fm->history[fm->hist_pos] = strdup(new_path);
        fm->hist_count = fm->hist_pos + 1;
    }

    fileview_populate(fm);
    navbar_update(fm);
}

void fm_refresh(Fm *fm)
{
    fm_dismiss_context(fm);

    if (fm->iconview) {
        IswIconViewSetItems(fm->iconview, NULL, NULL, 0);
    }

    browser_read_dir(fm, fm->cwd);
    fileview_populate(fm);
    navbar_update(fm);
}

/* ---------- keyboard shortcuts ---------- */

static void act_copy(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm) {
        clipboard_copy(fm);
    }
}

static void act_cut(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm) {
        clipboard_cut(fm);
    }
}

static void act_paste(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm) {
        clipboard_paste(fm);
    }
}

static void act_delete(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm) {
        fm_delete_selected(fm);
    }
}

static void act_delete_permanent(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm) {
        fm_delete_selected_permanent(fm);
    }
}

static void act_rename(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm) {
        show_rename_dialog(fm);
    }
}

static void act_go_up(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (!fm) {
        return;
    }
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

static void act_go_back(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (!fm || fm->hist_pos <= 0) {
        return;
    }
    fm->hist_pos--;
    free(fm->cwd);
    fm->cwd = strdup(fm->history[fm->hist_pos]);
    fm_refresh(fm);
}

static void act_go_fwd(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (!fm || fm->hist_pos >= fm->hist_count - 1) {
        return;
    }
    fm->hist_pos++;
    free(fm->cwd);
    fm->cwd = strdup(fm->history[fm->hist_pos]);
    fm_refresh(fm);
}

static void act_refresh(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm) {
        fm_refresh(fm);
    }
}

static void act_open(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (!fm || !fm->iconview) {
        return;
    }
    int sel = IswIconViewGetSelected(fm->iconview);
    if (sel >= 0 && sel < fm->nentries) {
        browser_open_entry(fm, sel);
    }
}

static void act_toggle_hidden(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (!fm) {
        return;
    }
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
    {"fm-new-window", act_new_window},
    {"fm-close-window", act_close_window},
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
    "Ctrl<Key>h:        fm-toggle-hidden()\n"
    "Ctrl<Key>n:        fm-new-window()\n";

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
    FmApp *app = fm->app_state;
    if (strcmp(section, "general") == 0 ||
        strcmp(section, "input") == 0 ||
        strcmp(section, "appearance") == 0 ||
        strcmp(section, "*") == 0) {
        if (strcmp(section, "appearance") == 0 || strcmp(section, "*") == 0) {
            isde_theme_reload();
            icons_init(app);
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

/* WM_DELETE_WINDOW action — close just this window */
static void act_close_window(Widget w, xcb_generic_event_t *ev,
                             String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm) {
        fm_window_destroy(fm);
    }
}

static void fm_destroy_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
    /* Remove from window list (may trigger app exit) but don't
     * double-free — the widget tree is already being destroyed. */
    FmApp *app = fm->app_state;
    app_remove_window(app, fm);
    /* Free per-window state (widgets are destroyed by Xt) */
    fm_dismiss_context(fm);
    ctx_free_dynamic(fm);
    dnd_cleanup(fm);
    clipboard_cleanup(fm);
    browser_free_entries(fm);
    fileview_cleanup(fm);
    places_cleanup(fm);
    free(fm->cwd);
    for (int i = 0; i < fm->hist_count; i++) {
        free(fm->history[i]);
    }
    free(fm);
}

/* ---------- window tracking ---------- */

static void app_add_window(FmApp *app, Fm *fm)
{
    app->windows = realloc(app->windows,
                           (app->nwindows + 1) * sizeof(Fm *));
    app->windows[app->nwindows++] = fm;
}

static void app_remove_window(FmApp *app, Fm *fm)
{
    for (int i = 0; i < app->nwindows; i++) {
        if (app->windows[i] == fm) {
            app->windows[i] = app->windows[--app->nwindows];
            break;
        }
    }
    if (app->nwindows == 0) {
        app->running = 0;
        XtAppSetExitFlag(app->app);
    }
}

/* ---------- per-window config ---------- */

static void load_window_config(Fm *fm)
{
    fm->double_click = 1;
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
}

/* ---------- new window action (Ctrl+N) ---------- */

static void act_new_window(Widget w, xcb_generic_event_t *ev,
                           String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (fm) {
        fm_window_new(fm->app_state, fm->cwd);
    }
}

/* ---------- app init / run / cleanup ---------- */

int fm_app_init(FmApp *app, int *argc, char **argv)
{
    memset(app, 0, sizeof(*app));

    /* Initialize context key for fm_from_widget lookups */
    fm_window_context = IswUniqueContext();

    char **fallbacks = isde_theme_build_resources();
    app->first_toplevel = XtAppInitialize(&app->app, "ISDE-FM",
                                          NULL, 0, argc, argv,
                                          fallbacks, NULL, 0);

    /* Register actions globally (once) */
    XtAppAddActions(app->app, fm_actions, XtNumber(fm_actions));

    /* Determine path to open */
    {
        const char *path = NULL;
        for (int i = 1; i < *argc; i++) {
            if (argv[i] && argv[i][0] != '-') {
                path = argv[i];
                break;
            }
        }
        if (!path) {
            const char *home = getenv("HOME");
            path = home ? home : "/";
        }
        app->initial_path = strdup(path);
    }

    /* Background file operations */
    jobqueue_init(app);

    /* Shared caches */
    icons_init(app);
    {
        static const char *app_dirs[] = {
            "/usr/share/applications",
            "/usr/local/share/applications",
            NULL
        };
        const char *home_env = getenv("HOME");
        char local_apps[512] = "";
        if (home_env) {
            snprintf(local_apps, sizeof(local_apps),
                     "%s/.local/share/applications", home_env);
        }

        int cap = 0;
        for (int d = -1; app_dirs[d + 1] || d < 0; d++) {
            const char *dir = (d < 0) ? local_apps : app_dirs[d];
            if (!dir[0]) {
                continue;
            }
            int count = 0;
            IsdeDesktopEntry **batch = isde_desktop_scan_dir(dir, &count);
            if (!batch) {
                continue;
            }
            if (app->ndesktop + count > cap) {
                cap = app->ndesktop + count + 64;
                app->desktop_entries = realloc(app->desktop_entries,
                                               cap * sizeof(IsdeDesktopEntry *));
            }
            for (int i = 0; i < count; i++) {
                app->desktop_entries[app->ndesktop++] = batch[i];
            }
            free(batch);
        }
    }

    /* D-Bus settings notifications (shared) */
    app->dbus = isde_dbus_init();
    if (app->dbus) {
        int dbus_fd = isde_dbus_get_fd(app->dbus);
        if (dbus_fd >= 0) {
            XtAppAddInput(app->app, dbus_fd,
                          (XtPointer)XtInputReadMask,
                          dbus_input_cb, app->dbus);
        }
    }

    /* Open initial window */
    Fm *first = fm_window_new(app, app->initial_path);
    if (!first) {
        return -1;
    }

    /* Single-instance check — now that the first window is realized,
     * use its toplevel for the selection ownership. */
    int rc = instance_try_primary(app, app->initial_path);
    if (rc == 0) {
        /* Another instance will handle the path — tear down and exit */
        fm_window_destroy(first);
        free(app->initial_path);
        app->initial_path = NULL;
        XtDestroyApplicationContext(app->app);
        return 1;
    }

    /* Subscribe D-Bus settings for first window (TODO: broadcast to all) */
    if (app->dbus) {
        isde_dbus_settings_subscribe(app->dbus, on_settings_changed, first);
    }

    app->running = 1;
    return 0;
}

Fm *fm_window_new(FmApp *app, const char *path)
{
    Fm *fm = calloc(1, sizeof(Fm));
    if (!fm) {
        return NULL;
    }
    fm->app_state = app;
    fm->rename_index = -1;
    fm->last_click_index = -1;

    load_window_config(fm);

    /* Create toplevel shell — first window reuses XtAppInitialize's shell,
     * subsequent windows create new application shells. */
    int fm_w = isde_scale(700);
    int fm_h = isde_scale(500);
    isde_clamp_to_workarea(XtDisplay(app->first_toplevel), 0, &fm_w, &fm_h);

    if (app->nwindows == 0) {
        fm->toplevel = app->first_toplevel;
    } else {
        Arg args[20];
        Cardinal n = 0;
        XtSetArg(args[n], XtNwidth, fm_w);                n++;
        XtSetArg(args[n], XtNheight, fm_h);               n++;
        XtSetArg(args[n], XtNminWidth, isde_scale(400));  n++;
        XtSetArg(args[n], XtNminHeight, isde_scale(300)); n++;
        fm->toplevel = XtAppCreateShell("isde-fm", "ISDE-FM",
                                        applicationShellWidgetClass,
                                        XtDisplay(app->first_toplevel),
                                        args, n);
    }

    if (app->nwindows == 0) {
        Arg args[20];
        Cardinal n = 0;
        XtSetArg(args[n], XtNwidth, fm_w);                n++;
        XtSetArg(args[n], XtNheight, fm_h);               n++;
        XtSetArg(args[n], XtNminWidth, isde_scale(400));  n++;
        XtSetArg(args[n], XtNminHeight, isde_scale(300)); n++;
        XtSetValues(fm->toplevel, args, n);
    }

    XtAddCallback(fm->toplevel, XtNdestroyCallback, fm_destroy_cb, fm);

    /* Override Shell's default WM_DELETE_WINDOW handler (which calls
     * XtAppSetExitFlag, killing all windows) with one that only
     * closes this window. */
    XtOverrideTranslations(fm->toplevel, XtParseTranslationTable(
        "<Message>WM_PROTOCOLS: fm-close-window()\n"));

    /* MainWindow */
    fm->main_window = XtCreateManagedWidget("mainWin", mainWindowWidgetClass,
                                            fm->toplevel, NULL, 0);
    XtUnmanageChild(IswMainWindowGetMenuBar(fm->main_window));

    /* Outer FlexBox: vertical */
    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNorientation, XtorientVertical); n++;
    XtSetArg(args[n], XtNborderWidth, 0);                 n++;
    fm->vbox = XtCreateManagedWidget("vbox", flexBoxWidgetClass,
                                      fm->main_window, args, n);

    navbar_init(fm);

    /* Content area: horizontal FlexBox */
    n = 0;
    XtSetArg(args[n], XtNorientation, XtorientHorizontal); n++;
    XtSetArg(args[n], XtNborderWidth, 0);                   n++;
    XtSetArg(args[n], XtNflexGrow, 1);                      n++;
    fm->hbox = XtCreateManagedWidget("hbox", flexBoxWidgetClass,
                                      fm->vbox, args, n);

    places_init(fm);
    fileview_init(fm);
    clipboard_init(fm);

    /* Navigate to initial path */
    fm->cwd = strdup(path);
    fm->history[0] = strdup(path);
    fm->hist_pos = 0;
    fm->hist_count = 1;

    browser_read_dir(fm, fm->cwd);

    XtRealizeWidget(fm->toplevel);

    /* Store Fm* for fm_from_widget lookups */
    fm_set_context(fm->toplevel, fm);

    /* XDND init must be after realize */
    dnd_init(fm);

    fileview_populate(fm);
    navbar_update(fm);

    app_add_window(app, fm);
    return fm;
}

void fm_window_destroy(Fm *fm)
{
    /* All actual cleanup happens in fm_destroy_cb (the XtNdestroyCallback).
     * This just triggers the widget destruction. */
    XtDestroyWidget(fm->toplevel);
}

void fm_app_run(FmApp *app)
{
    while (app->running && !XtAppGetExitFlag(app->app)) {
        XtAppProcessEvent(app->app, XtIMAll);
    }
}

void fm_app_cleanup(FmApp *app)
{
    /* Destroy any remaining windows */
    while (app->nwindows > 0) {
        Fm *fm = app->windows[0];
        XtDestroyWidget(fm->toplevel);
    }
    free(app->windows);

    jobqueue_shutdown(app);
    isde_dbus_free(app->dbus);
    icons_cleanup(app);
    free(app->initial_path);
    for (int i = 0; i < app->ndesktop; i++) {
        isde_desktop_free(app->desktop_entries[i]);
    }
    free(app->desktop_entries);
    XtDestroyApplicationContext(app->app);
}
