#define _POSIX_C_SOURCE 200809L
/*
 * context.c — right-click context menu
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ISW/ISWRender.h>

#include <ISW/IswArgMacros.h>
#include "isde/isde-dialog.h"

/* ---------- context menu action wrappers ---------- */

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
    int *indices = NULL;
    int nsel = fileview_get_selected_items(fm, &indices);
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

static void ctx_properties(Fm *fm) { show_properties_dialog(fm); }

static String base_labels[] = {
    "Cut", "Copy", "Paste", "---",
    "Rename", "Delete", "---",
    "New Folder", "Open Terminal Here", "---",
    "Properties"
};
static CtxAction base_actions[] = {
    ctx_cut, ctx_copy, ctx_paste, NULL,
    ctx_rename, ctx_delete_action, NULL,
    ctx_new_folder, ctx_open_terminal, NULL,
    ctx_properties
};
#define BASE_NITEMS 11

static String ctx_trash_labels[] = {
    "Restore", "Delete Permanently", "---", "Empty Trash", NULL
};
static CtxAction ctx_trash_actions[] = {
    ctx_restore, ctx_delete_action, NULL, ctx_empty_trash
};
#define CTX_TRASH_NITEMS 4

void ctx_free_dynamic(Fm *fm)
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
    fm->ow_default = -1;
    free(fm->ow_file_path);
    fm->ow_file_path = NULL;
    free(fm->ow_mime);
    fm->ow_mime = NULL;
}

void fm_dismiss_context(Fm *fm)
{
    if (fm->ctx_shell) {
        IswPopdown(fm->ctx_shell);
        IswDestroyWidget(fm->ctx_shell);
        fm->ctx_shell = NULL;
        fm->ctx_list = NULL;
    }
    ctx_free_dynamic(fm);
    places_dismiss_device_menu(fm);
}

static void ctx_run_action(Fm *fm, int action_idx)
{
    FmApp *app = fm->app_state;
    if (action_idx < 0 || action_idx >= app->nactions)
        return;

    FmAction *act = &app->actions[action_idx];

    int *sel_indices = NULL;
    int nsel = fileview_get_selected_items(fm, &sel_indices);
    if (nsel <= 0) {
        free(sel_indices);
        return;
    }

    /* Build argv: [script_path, file1, file2, ..., NULL] */
    char **argv = malloc((nsel + 2) * sizeof(char *));
    argv[0] = act->script_path;
    for (int i = 0; i < nsel; i++)
        argv[i + 1] = fm->entries[sel_indices[i]].full_path;
    argv[nsel + 1] = NULL;

    pid_t pid = fork();
    if (pid == 0) {
        execv(act->script_path, argv);
        _exit(127);
    }

    free(argv);
    free(sel_indices);
}

static void ctx_select_cb(Widget w, IswPointer client_data,
                          IswPointer call_data)
{
    (void)w;
    (void)client_data;
    IswListReturnStruct *ret = (IswListReturnStruct *)call_data;
    int idx = ret->list_index;

    Fm *fm = fm_from_widget(w);
    if (!fm) {
        return;
    }
    int in_trash = fm->ctx_in_trash;
    int target = fm->ctx_target_index;

    if (idx < 0) {
        fm_dismiss_context(fm);
        fm->ctx_target_index = -1;
        return;
    }

    if (in_trash) {
        fm_dismiss_context(fm);
        fm->ctx_target_index = target;
        if (idx < CTX_TRASH_NITEMS && ctx_trash_actions[idx]) {
            ctx_trash_actions[idx](fm);
        }
        fm->ctx_target_index = -1;
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
            isde_desktop_launch(de, (const char **)&file, 1);
            free(file);
        }
        fm->ctx_target_index = -1;
        return;
    }

    /* Check if this is a custom script action */
    if (fm->ctx_naction_indices > 0 &&
        idx >= fm->ctx_action_offset &&
        idx < fm->ctx_action_offset + fm->ctx_naction_indices) {
        int ai = fm->ctx_action_indices[idx - fm->ctx_action_offset];
        fm->ctx_target_index = target;
        ctx_run_action(fm, ai);
        fm_dismiss_context(fm);
        fm->ctx_target_index = -1;
        return;
    }

    CtxAction action = NULL;
    if (idx >= 0 && idx < fm->dyn_nitems)
        action = fm->dyn_actions[idx];

    fm->ctx_target_index = target;

    if (action)
        action(fm);

    fm_dismiss_context(fm);
    fm->ctx_target_index = -1;
}

static int ow_entry_eligible(IsdeDesktopEntry *de, const char *mime)
{
    if (!de) return 0;
    if (isde_desktop_hidden(de) || isde_desktop_no_display(de)) return 0;
    if (!isde_desktop_handles_mime(de, mime)) return 0;
    if (!isde_desktop_name(de)) return 0;
    return 1;
}

static void ctx_build_open_with(Fm *fm)
{
    fm->ow_count = 0;
    fm->ow_default = -1;
    FmApp *app = fm->app_state;

    int sel = fileview_get_selected(fm);
    if (sel < 0 || sel >= fm->nentries)
        return;

    FmEntry *e = &fm->entries[sel];
    if (e->is_dir)
        return;

    const char *mime = isde_mime_type_for_file(e->name);
    if (!mime || strcmp(mime, "application/octet-stream") == 0)
        return;

    free(fm->ow_file_path);
    fm->ow_file_path = strdup(e->full_path);
    free(fm->ow_mime);
    fm->ow_mime = strdup(mime);

    char *default_id = isde_mime_default_app(mime);
    int default_de_idx = -1;

    if (default_id) {
        for (int i = 0; i < app->ndesktop; i++) {
            IsdeDesktopEntry *de = app->desktop_entries[i];
            if (!de) continue;
            const char *id = isde_desktop_id(de);
            if (id && strcmp(id, default_id) == 0 &&
                ow_entry_eligible(de, mime)) {
                default_de_idx = i;
                break;
            }
        }
        free(default_id);
    }

    /* Place default app first */
    if (default_de_idx >= 0) {
        IsdeDesktopEntry *de = app->desktop_entries[default_de_idx];
        fm->ow_indices[0] = default_de_idx;
        char buf[256];
        snprintf(buf, sizeof(buf), "Open with %s (default)", isde_desktop_name(de));
        fm->ow_label_buf[0] = strdup(buf);
        fm->ow_default = 0;
        fm->ow_count = 1;
    }

    /* Remaining matching apps */
    for (int i = 0; i < app->ndesktop && fm->ow_count < MAX_OPEN_WITH; i++) {
        if (i == default_de_idx) continue;
        IsdeDesktopEntry *de = app->desktop_entries[i];
        if (!ow_entry_eligible(de, mime)) continue;

        fm->ow_indices[fm->ow_count] = i;
        char buf[256];
        snprintf(buf, sizeof(buf), "Open with %s", isde_desktop_name(de));
        fm->ow_label_buf[fm->ow_count] = strdup(buf);
        fm->ow_count++;
    }
}

static void ctx_build_actions(Fm *fm)
{
    fm->ctx_naction_indices = 0;
    fm->ctx_action_offset = 0;
    FmApp *app = fm->app_state;
    if (app->nactions == 0)
        return;

    /* Collect selected entries */
    int *sel = NULL;
    int nsel = fileview_get_selected_items(fm, &sel);
    if (nsel <= 0) {
        free(sel);
        /* No selection — only show filterless actions */
        const FmEntry *none = NULL;
        fm->ctx_naction_indices = actions_match(app, &none, 0,
                                                fm->ctx_action_indices,
                                                MAX_CTX_ACTIONS);
        return;
    }

    const FmEntry **entries = malloc(nsel * sizeof(FmEntry *));
    for (int i = 0; i < nsel; i++)
        entries[i] = &fm->entries[sel[i]];

    fm->ctx_naction_indices = actions_match(app, entries, nsel,
                                            fm->ctx_action_indices,
                                            MAX_CTX_ACTIONS);
    free(entries);
    free(sel);
}

static void ctx_build_menu(Fm *fm)
{
    FmApp *app = fm->app_state;

    ctx_build_actions(fm);

    int ow_section = 0;
    if (fm->ow_count > 0)
        ow_section = fm->ow_count + 2; /* entries + "Set Default..." + separator */

    int act_section = 0;
    if (fm->ctx_naction_indices > 0)
        act_section = fm->ctx_naction_indices + 1; /* entries + separator */

    int total = ow_section + act_section + BASE_NITEMS;

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
        fm->dyn_labels[pos] = "Set Default Application...";
        fm->dyn_actions[pos] = ctx_set_default;
        pos++;
        fm->dyn_labels[pos] = "---";
        fm->dyn_actions[pos] = NULL;
        pos++;
    }

    /* Custom script actions */
    fm->ctx_action_offset = pos;
    for (int i = 0; i < fm->ctx_naction_indices; i++) {
        fm->dyn_labels[pos] = app->actions[fm->ctx_action_indices[i]].name;
        fm->dyn_actions[pos] = NULL;
        pos++;
    }
    if (fm->ctx_naction_indices > 0) {
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

static void ctx_handler(Widget w, IswPointer client_data,
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

    Widget view = (fm->view_mode == FM_VIEW_LIST) ? fm->listview : fm->iconview;
    xcb_connection_t *conn = IswDisplay(view);
    xcb_translate_coordinates_cookie_t tc =
        xcb_translate_coordinates(conn, ev->event, IswWindow(view),
                                  ev->event_x, ev->event_y);
    xcb_translate_coordinates_reply_t *tr =
        xcb_translate_coordinates_reply(conn, tc, NULL);
    fm->ctx_target_index = -1;
    if (tr) {
        int vx = tr->dst_x;
        int vy = tr->dst_y;
        free(tr);
        fm->ctx_target_index = (fm->view_mode == FM_VIEW_LIST)
            ? IswListViewHitTest(fm->listview, vx, vy)
            : IswIconViewHitTest(fm->iconview, vx, vy);
    }

    /* Check if we're in trash */
    char *trash_path = fileops_trash_path();
    fm->ctx_in_trash = (strncmp(fm->cwd, trash_path, strlen(trash_path)) == 0);
    free(trash_path);

    if (fm->ctx_in_trash) {
        fm->dyn_labels = NULL;
        fm->dyn_actions = NULL;
        fm->dyn_nitems = CTX_TRASH_NITEMS;
    } else {
        ctx_build_open_with(fm);
        ctx_build_menu(fm);
    }

    double sf = ISWScaleFactor(fm->toplevel);
    Position px = ev->root_x;
    Position py = ev->root_y;

    static char ctxTranslations[] =
        "<EnterWindow>: Set()\n"
        "<LeaveWindow>: Unset()\n"
        "<Motion>:      Set()\n"
        "<BtnDown>:     Set() Notify()\n"
        "<BtnUp>:       Notify()";

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgOverrideRedirect(&ab, True);
    fm->ctx_shell = IswCreatePopupShell("ctxMenu", overrideShellWidgetClass,
                                   fm->toplevel, ab.args, ab.count);

    String *labels = fm->ctx_in_trash ? ctx_trash_labels : fm->dyn_labels;
    int nitems = fm->ctx_in_trash ? CTX_TRASH_NITEMS : fm->dyn_nitems;

    IswArgBuilderReset(&ab);
    IswArgList(&ab, labels);
    IswArgNumberStrings(&ab, nitems);
    IswArgDefaultColumns(&ab, 1);
    IswArgForceColumns(&ab, True);
    IswArgVerticalList(&ab, True);
    IswArgBorderWidth(&ab, 0);
    IswArgCursor(&ab, None);
    fm->ctx_list = IswCreateManagedWidget("ctxList", listWidgetClass,
                                     fm->ctx_shell, ab.args, ab.count);
    IswAddCallback(fm->ctx_list, IswNcallback, ctx_select_cb, NULL);
    IswOverrideTranslations(fm->ctx_list,
                           IswParseTranslationTable(ctxTranslations));

    IswRealizeWidget(fm->ctx_shell);

    xcb_screen_t *scr = IswScreen(fm->toplevel);
    int scr_w = (int)(scr->width_in_pixels / sf);
    int scr_h = (int)(scr->height_in_pixels / sf);
    Dimension mw = fm->ctx_shell->core.width;
    Dimension mh = fm->ctx_shell->core.height;
    Dimension bw = fm->ctx_shell->core.border_width;
    int menu_w = (int)mw + 2 * (int)bw;
    int menu_h = (int)mh + 2 * (int)bw;

    if (menu_h > scr_h) {
        IswDestroyWidget(fm->ctx_shell);
        fm->ctx_shell = NULL;
        fm->ctx_list = NULL;

        int max_h = scr_h - 16;

        IswArgBuilderReset(&ab);
        IswArgOverrideRedirect(&ab, True);
        fm->ctx_shell = IswCreatePopupShell("ctxMenu", overrideShellWidgetClass,
                                       fm->toplevel, ab.args, ab.count);

        IswArgBuilderReset(&ab);
        IswArgAllowVert(&ab, True);
        IswArgUseRight(&ab, True);
        IswArgHeight(&ab, (Dimension)max_h);
        Widget vp = IswCreateManagedWidget("ctxVp", viewportWidgetClass,
                                            fm->ctx_shell, ab.args, ab.count);

        IswArgBuilderReset(&ab);
        IswArgList(&ab, labels);
        IswArgNumberStrings(&ab, nitems);
        IswArgDefaultColumns(&ab, 1);
        IswArgForceColumns(&ab, True);
        IswArgVerticalList(&ab, True);
        IswArgBorderWidth(&ab, 0);
        IswArgCursor(&ab, None);
        fm->ctx_list = IswCreateManagedWidget("ctxList", listWidgetClass,
                                         vp, ab.args, ab.count);
        IswAddCallback(fm->ctx_list, IswNcallback, ctx_select_cb, NULL);
        IswOverrideTranslations(fm->ctx_list,
                               IswParseTranslationTable(ctxTranslations));

        IswRealizeWidget(fm->ctx_shell);

        mw = fm->ctx_shell->core.width;
        mh = fm->ctx_shell->core.height;
        bw = fm->ctx_shell->core.border_width;
        menu_w = (int)mw + 2 * (int)bw;
        menu_h = (int)mh + 2 * (int)bw;
    }

    Position rx = px + 1;
    Position ry = py;
    if ((int)rx + menu_w > scr_w)
        rx = (Position)((int)px - menu_w);
    if ((int)ry + menu_h > scr_h)
        ry = (Position)(scr_h - menu_h);
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;

    IswMoveWidget(fm->ctx_shell, rx, ry);
    IswPopup(fm->ctx_shell, IswGrabNone);
}

void fm_register_context_menu(Fm *fm, Widget w)
{
    IswAddEventHandler(w, XCB_EVENT_MASK_BUTTON_PRESS, False, ctx_handler, fm);
}
