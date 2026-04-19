#define _POSIX_C_SOURCE 200809L
/*
 * fm.c — app init, menu setup, navigation logic
 */
#include "fm.h"

#include <stdio.h>
#include "isde/isde-ewmh.h"
#include "isde/isde-dialog.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/inotify.h>
#endif
#include <ISW/IswArgMacros.h>

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

static void fm_delete_selected(Fm *fm)
{
    fm_delete_confirm(fm, 0);
}

static void fm_delete_selected_permanent(Fm *fm)
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

static void ctx_empty_trash(Fm *fm)
{
    isde_dialog_dismiss(fm->empty_trash_shell);
    fm->empty_trash_shell = isde_dialog_confirm(
        fm->toplevel, "Empty Trash",
        "Permanently delete all items in Trash?",
        "Empty Trash", empty_trash_result_cb, fm);
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
        IswPopdown(fm->ctx_shell);
        IswDestroyWidget(fm->ctx_shell);
        fm->ctx_shell = NULL;
        fm->ctx_list = NULL;
    }
    ctx_free_dynamic(fm);
    places_dismiss_device_menu(fm);
}

static void ctx_select_cb(Widget w, IswPointer client_data,
                          IswPointer call_data)
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

    int sel = fileview_get_selected(fm);
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

    Position rx = ev->root_x;
    Position ry = ev->root_y;

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgX(&ab, rx);
    IswArgY(&ab, ry);
    IswArgOverrideRedirect(&ab, True);
    IswArgBorderWidth(&ab, 1);
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

    static char ctxTranslations[] =
        "<EnterWindow>: Set()\n"
        "<LeaveWindow>: Unset()\n"
        "<Motion>:      Set()\n"
        "<BtnDown>:     Set() Notify()\n"
        "<BtnUp>:       Notify()";
    IswOverrideTranslations(fm->ctx_list,
                           IswParseTranslationTable(ctxTranslations));

    IswPopup(fm->ctx_shell, IswGrabNone);
}

void fm_register_context_menu(Fm *fm, Widget w)
{
    IswAddEventHandler(w, XCB_EVENT_MASK_BUTTON_PRESS, False, ctx_handler, fm);
}

/* ---------- navigation ---------- */

void fm_navigate(Fm *fm, const char *path)
{
    fm_dismiss_context(fm);

    char *new_path = strdup(path);

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
    if (!fm) {
        return;
    }
    int sel = fileview_get_selected(fm);
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

static void act_toggle_view(Widget w, xcb_generic_event_t *ev, String *p, Cardinal *n)
{
    (void)ev; (void)p; (void)n;
    Fm *fm = fm_from_widget(w);
    if (!fm) {
        return;
    }
    fileview_set_mode(fm, fm->view_mode == FM_VIEW_ICON
                         ? FM_VIEW_LIST : FM_VIEW_ICON);
}

static IswActionsRec fm_actions[] = {
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
    {"fm-toggle-view", act_toggle_view},
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
    "Ctrl<Key>l:        fm-toggle-view()\n"
    "Ctrl<Key>n:        fm-new-window()\n";

void fm_install_shortcuts(Widget w)
{
    IswOverrideTranslations(w, IswParseTranslationTable(fm_translations));
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

static void dbus_input_cb(IswPointer client_data, int *fd, IswInputId *id)
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

static void fm_destroy_cb(Widget w, IswPointer cd, IswPointer call)
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
        IswAppSetExitFlag(app->app);
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

/* ---------- mount monitor ---------- */

#ifdef __linux__
static void mount_changed_cb(IswPointer cd, int *fd, IswInputId *id)
{
    (void)id;
    FmApp *app = (FmApp *)cd;

    char buf[4096]
        __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t len;

    while ((len = read(*fd, buf, sizeof(buf))) > 0) {
        for (char *ptr = buf; ptr < buf + len; ) {
            struct inotify_event *ev = (struct inotify_event *)ptr;
            if (ev->len == 0) {
                ptr += sizeof(*ev) + ev->len;
                continue;
            }

            const char *name = ev->name;
            const char *user = getenv("USER");
            char path[512];
            snprintf(path, sizeof(path), "/media/%s/%s",
                     user ? user : "", name);

            fprintf(stderr, "isde-fm: mount event: %s %s\n",
                    (ev->mask & IN_CREATE) ? "CREATE" : "DELETE", name);

            for (int i = 0; i < app->nwindows; i++) {
                Fm *fm = app->windows[i];
                if (ev->mask & IN_CREATE) {
                    places_device_added(fm, name, path);
                } else if (ev->mask & IN_DELETE) {
                    places_device_removed(fm, name);

                    /* Navigate away if viewing a removed mount */
                    if (strncmp(fm->cwd, path, strlen(path)) == 0) {
                        const char *home = getenv("HOME");
                        fm_navigate(fm, home ? home : "/");
                    }
                }
            }

            ptr += sizeof(*ev) + ev->len;
        }
    }
}

static void mount_monitor_init(FmApp *app)
{
    const char *user = getenv("USER");
    if (!user) {
        return;
    }

    char media_path[256];
    snprintf(media_path, sizeof(media_path), "/media/%s", user);

    struct stat st;
    if (stat(media_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return;
    }

    app->mount_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (app->mount_inotify_fd < 0) {
        return;
    }

    app->mount_wd = inotify_add_watch(app->mount_inotify_fd,
                                       media_path,
                                       IN_CREATE | IN_DELETE);
    if (app->mount_wd < 0) {
        close(app->mount_inotify_fd);
        app->mount_inotify_fd = -1;
        return;
    }

    app->mount_input_id = IswAppAddInput(app->app, app->mount_inotify_fd,
                                          (IswPointer)IswInputReadMask,
                                          mount_changed_cb, app);
    fprintf(stderr, "isde-fm: mount monitor watching %s (fd=%d)\n",
            media_path, app->mount_inotify_fd);
}

static void mount_monitor_cleanup(FmApp *app)
{
    if (app->mount_inotify_fd >= 0) {
        if (app->mount_input_id) {
            IswRemoveInput(app->mount_input_id);
        }
        close(app->mount_inotify_fd);
        app->mount_inotify_fd = -1;
    }
}
#endif /* __linux__ */

/* ---------- app init / run / cleanup ---------- */

int fm_app_init(FmApp *app, int *argc, char **argv)
{
    memset(app, 0, sizeof(*app));
    app->mount_inotify_fd = -1;

    /* Initialize context key for fm_from_widget lookups */
    fm_window_context = IswUniqueContext();

    char **fallbacks = isde_theme_build_resources();
    app->first_toplevel = IswAppInitialize(&app->app, "ISDE-FM",
                                          NULL, 0, argc, argv,
                                          fallbacks, NULL, 0);

    /* Register actions globally (once) */
    IswAppAddActions(app->app, fm_actions, IswNumber(fm_actions));

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
            IswAppAddInput(app->app, dbus_fd,
                          (IswPointer)IswInputReadMask,
                          dbus_input_cb, app->dbus);
        }
    }

    /* Connect to mountd before creating windows so the sidebar
     * can show all devices (mounted and unmounted). */
    fm_mountd_init(app);

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
        IswDestroyApplicationContext(app->app);
        return 1;
    }

    /* Subscribe D-Bus settings for first window (TODO: broadcast to all) */
    if (app->dbus) {
        isde_dbus_settings_subscribe(app->dbus, on_settings_changed, first);
    }

#ifdef __linux__
    /* Only use inotify mount monitor when mountd is not available —
     * mountd signals handle device changes when it's running. */
    if (!app->has_mountd)
        mount_monitor_init(app);
#endif

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

    /* Create toplevel shell — first window reuses IswAppInitialize's shell,
     * subsequent windows create new application shells. */
    int fm_w = 700;
    int fm_h = 500;
    isde_clamp_to_workarea(IswDisplay(app->first_toplevel), 0, &fm_w, &fm_h);

    if (app->nwindows == 0) {
        fm->toplevel = app->first_toplevel;
    } else {
        IswArgBuilder ab = IswArgBuilderInit();
        IswArgWidth(&ab, fm_w);
        IswArgHeight(&ab, fm_h);
        IswArgMinWidth(&ab, 400);
        IswArgMinHeight(&ab, 300);
        fm->toplevel = IswAppCreateShell("isde-fm", "ISDE-FM",
                                        applicationShellWidgetClass,
                                        IswDisplay(app->first_toplevel),
                                        ab.args, ab.count);
    }

    if (app->nwindows == 0) {
        IswArgBuilder ab = IswArgBuilderInit();
        IswArgWidth(&ab, fm_w);
        IswArgHeight(&ab, fm_h);
        IswArgMinWidth(&ab, 400);
        IswArgMinHeight(&ab, 300);
        IswSetValues(fm->toplevel, ab.args, ab.count);
    }

    IswAddCallback(fm->toplevel, IswNdestroyCallback, fm_destroy_cb, fm);

    /* Override Shell's default WM_DELETE_WINDOW handler (which calls
     * IswAppSetExitFlag, killing all windows) with one that only
     * closes this window. */
    IswOverrideTranslations(fm->toplevel, IswParseTranslationTable(
        "<Message>WM_PROTOCOLS: fm-close-window()\n"));

    /* MainWindow */
    fm->main_window = IswCreateManagedWidget("mainWin", mainWindowWidgetClass,
                                            fm->toplevel, NULL, 0);
    IswUnmanageChild(IswMainWindowGetMenuBar(fm->main_window));

    /* Outer FlexBox: vertical */
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgOrientation(&ab, XtorientVertical);
    IswArgBorderWidth(&ab, 0);
    fm->vbox = IswCreateManagedWidget("vbox", flexBoxWidgetClass,
                                      fm->main_window, ab.args, ab.count);

    navbar_init(fm);

    /* Content area: horizontal FlexBox */
    IswArgBuilderReset(&ab);
    IswArgOrientation(&ab, XtorientHorizontal);
    IswArgBorderWidth(&ab, 0);
    IswArgFlexGrow(&ab, 1);
    fm->hbox = IswCreateManagedWidget("hbox", flexBoxWidgetClass,
                                      fm->vbox, ab.args, ab.count);

    places_init(fm);
    fileview_init(fm);
    clipboard_init(fm);

    /* Navigate to initial path */
    fm->cwd = strdup(path);
    fm->history[0] = strdup(path);
    fm->hist_pos = 0;
    fm->hist_count = 1;

    browser_read_dir(fm, fm->cwd);

    IswRealizeWidget(fm->toplevel);

    /* Store Fm* for fm_from_widget lookups */
    fm_set_context(fm->toplevel, fm);

    /* XDND init must be after realize — re-apply keyboard shortcuts
     * afterward since DnD translation overrides can clobber them. */
    dnd_init(fm);
    places_register_drop_targets(fm);
    fm_install_shortcuts(fm->iconview);
    fm_install_shortcuts(fm->listview);

    fileview_populate(fm);
    navbar_update(fm);

    app_add_window(app, fm);
    return fm;
}

void fm_window_destroy(Fm *fm)
{
    /* All actual cleanup happens in fm_destroy_cb (the IswNdestroyCallback).
     * This just triggers the widget destruction. */
    IswDestroyWidget(fm->toplevel);
}

void fm_app_run(FmApp *app)
{
    while (app->running && !IswAppGetExitFlag(app->app)) {
        IswAppProcessEvent(app->app, IswIMAll);
    }
}

void fm_app_cleanup(FmApp *app)
{
    /* Destroy any remaining windows */
    while (app->nwindows > 0) {
        Fm *fm = app->windows[0];
        IswDestroyWidget(fm->toplevel);
    }
    free(app->windows);

    fm_mountd_cleanup(app);
#ifdef __linux__
    mount_monitor_cleanup(app);
#endif
    jobqueue_shutdown(app);
    isde_dbus_free(app->dbus);
    icons_cleanup(app);
    free(app->initial_path);
    for (int i = 0; i < app->ndesktop; i++) {
        isde_desktop_free(app->desktop_entries[i]);
    }
    free(app->desktop_entries);
    IswDestroyApplicationContext(app->app);
}
