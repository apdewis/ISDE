#define _POSIX_C_SOURCE 200809L
/*
 * fm.c — app init, menu setup, navigation logic
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------- menu callbacks ---------- */

static void quit_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
    fm->running = 0;
}

static void go_back_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
    if (fm->hist_pos > 0) {
        fm->hist_pos--;
        free(fm->cwd);
        fm->cwd = strdup(fm->history[fm->hist_pos]);
        fm_refresh(fm);
    }
}

static void go_fwd_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
    if (fm->hist_pos < fm->hist_count - 1) {
        fm->hist_pos++;
        free(fm->cwd);
        fm->cwd = strdup(fm->history[fm->hist_pos]);
        fm_refresh(fm);
    }
}

static void go_up_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
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

static void go_home_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
    const char *home = getenv("HOME");
    if (home)
        fm_navigate(fm, home);
}

static void go_root_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    fm_navigate((Fm *)cd, "/");
}

static void view_icons_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    fileview_set_mode((Fm *)cd, FM_VIEW_ICON);
}

static void view_list_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    fileview_set_mode((Fm *)cd, FM_VIEW_LIST);
}

static void toggle_hidden_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
    fm->show_hidden = !fm->show_hidden;
    fm_refresh(fm);
}

static void new_folder_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
    fileops_mkdir(fm, "New Folder");
    fm_refresh(fm);
}

static void cut_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    clipboard_cut((Fm *)cd);
}

static void copy_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    clipboard_copy((Fm *)cd);
}

static void paste_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    clipboard_paste((Fm *)cd);
}

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

    n = 0;
    XtSetArg(args[n], XtNlabel, "Rename:");         n++;
    XtSetArg(args[n], XtNvalue, fm->entries[sel].name); n++;
    Widget dialog = XtCreateManagedWidget("renameDialog", dialogWidgetClass,
                                          rename_shell, args, n);

    IswDialogAddButton(dialog, "OK", rename_ok_cb, (XtPointer)dialog);
    IswDialogAddButton(dialog, "Cancel", rename_cancel_cb, NULL);

    XtPopup(rename_shell, XtGrabNone);
}

static void rename_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    show_rename_dialog((Fm *)cd);
}

/* ---------- delete ---------- */

static void delete_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
    if (!fm->iconview) return;

    int *indices = NULL;
    int nsel = IswIconViewGetSelectedItems(fm->iconview, &indices);
    /* Delete in reverse order to keep indices valid */
    for (int i = nsel - 1; i >= 0; i--) {
        int idx = indices[i];
        if (idx >= 0 && idx < fm->nentries)
            fileops_delete(fm, fm->entries[idx].full_path);
    }
    free(indices);
    if (nsel > 0)
        fm_refresh(fm);
}

static void refresh_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    fm_refresh((Fm *)cd);
}

static void open_terminal_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
    pid_t pid = fork();
    if (pid == 0) {
        chdir(fm->cwd);
        execlp("xterm", "xterm", (char *)NULL);
        _exit(127);
    }
}

/* ---------- menu creation helpers ---------- */

static int menu_entry_id = 0;

static Widget add_menu_entry(Widget menu, const char *label,
                             XtCallbackProc cb, XtPointer cd)
{
    Arg args[20];
    char name[32];
    snprintf(name, sizeof(name), "entry%d", menu_entry_id++);
    XtSetArg(args[0], XtNlabel, label);
    Widget entry = XtCreateManagedWidget(name, smeBSBObjectClass,
                                         menu, args, 1);
    if (cb)
        XtAddCallback(entry, XtNcallback, cb, cd);
    return entry;
}

static void add_separator(Widget menu)
{
    XtCreateManagedWidget("sep", smeLineObjectClass, menu, NULL, 0);
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
    if (!fm->iconview) return;
    int *indices = NULL;
    int nsel = IswIconViewGetSelectedItems(fm->iconview, &indices);
    for (int i = nsel - 1; i >= 0; i--) {
        int idx = indices[i];
        if (idx >= 0 && idx < fm->nentries)
            fileops_delete(fm, fm->entries[idx].full_path);
    }
    free(indices);
    if (nsel > 0) fm_refresh(fm);
}
static void ctx_new_folder(Fm *fm) {
    fileops_mkdir(fm, "New Folder");
    fm_refresh(fm);
}

static String ctx_labels[] = {
    "Cut", "Copy", "Paste", "---",
    "Rename", "Delete", "---", "New Folder", NULL
};
static CtxAction ctx_actions[] = {
    ctx_cut, ctx_copy, ctx_paste, NULL,
    ctx_rename, ctx_delete_action, NULL, ctx_new_folder
};
#define CTX_NITEMS 8

void fm_dismiss_context(void)
{
    if (ctx_shell) {
        XtPopdown(ctx_shell);
        XtDestroyWidget(ctx_shell);
        ctx_shell = NULL;
        ctx_list = NULL;
    }
}

static void ctx_select_cb(Widget w, XtPointer client_data,
                          XtPointer call_data)
{
    (void)w;
    (void)client_data;
    IswListReturnStruct *ret = (IswListReturnStruct *)call_data;
    fm_dismiss_context();

    if (!ctx_fm || ret->list_index < 0 || ret->list_index >= CTX_NITEMS)
        return;
    CtxAction action = ctx_actions[ret->list_index];
    if (action)
        action(ctx_fm);
}

static void ctx_handler(Widget w, XtPointer client_data,
                        XEvent *event, Boolean *cont)
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
    XtSetArg(args[n], XtNlist, ctx_labels);       n++;
    XtSetArg(args[n], XtNnumberStrings, CTX_NITEMS); n++;
    XtSetArg(args[n], XtNdefaultColumns, 1);       n++;
    XtSetArg(args[n], XtNforceColumns, True);      n++;
    XtSetArg(args[n], XtNverticalList, True);      n++;
    XtSetArg(args[n], XtNborderWidth, 0);          n++;
    XtSetArg(args[n], XtNcursor, None);            n++;
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

static void setup_menus(Fm *fm, Widget menubar)
{
    Arg args[20];
    Cardinal n;

    /* File menu */
    n = 0;
    XtSetArg(args[n], XtNlabel, "File"); n++;
    XtSetArg(args[n], XtNmenuName, "fileMenu"); n++;
    XtCreateManagedWidget("fileBtn", menuButtonWidgetClass,
                          menubar, args, n);
    Widget file_menu = XtCreatePopupShell("fileMenu", simpleMenuWidgetClass,
                                          menubar, NULL, 0);
    add_menu_entry(file_menu, "New Folder", new_folder_cb, fm);
    add_menu_entry(file_menu, "Open Terminal Here", open_terminal_cb, fm);
    add_separator(file_menu);
    add_menu_entry(file_menu, "Refresh", refresh_cb, fm);
    add_separator(file_menu);
    add_menu_entry(file_menu, "Quit", quit_cb, fm);

    /* Edit menu */
    n = 0;
    XtSetArg(args[n], XtNlabel, "Edit"); n++;
    XtSetArg(args[n], XtNmenuName, "editMenu"); n++;
    XtCreateManagedWidget("editBtn", menuButtonWidgetClass,
                          menubar, args, n);
    Widget edit_menu = XtCreatePopupShell("editMenu", simpleMenuWidgetClass,
                                          menubar, NULL, 0);
    add_menu_entry(edit_menu, "Cut", cut_cb, fm);
    add_menu_entry(edit_menu, "Copy", copy_cb, fm);
    add_menu_entry(edit_menu, "Paste", paste_cb, fm);
    add_separator(edit_menu);
    add_menu_entry(edit_menu, "Rename", rename_cb, fm);
    add_menu_entry(edit_menu, "Delete", delete_cb, fm);

    /* View menu */
    n = 0;
    XtSetArg(args[n], XtNlabel, "View"); n++;
    XtSetArg(args[n], XtNmenuName, "viewMenu"); n++;
    XtCreateManagedWidget("viewBtn", menuButtonWidgetClass,
                          menubar, args, n);
    Widget view_menu = XtCreatePopupShell("viewMenu", simpleMenuWidgetClass,
                                          menubar, NULL, 0);
    add_menu_entry(view_menu, "Icons", view_icons_cb, fm);
    add_menu_entry(view_menu, "List", view_list_cb, fm);
    add_separator(view_menu);
    add_menu_entry(view_menu, "Show Hidden Files", toggle_hidden_cb, fm);

    /* Go menu */
    n = 0;
    XtSetArg(args[n], XtNlabel, "Go"); n++;
    XtSetArg(args[n], XtNmenuName, "goMenu"); n++;
    XtCreateManagedWidget("goBtn", menuButtonWidgetClass,
                          menubar, args, n);
    Widget go_menu = XtCreatePopupShell("goMenu", simpleMenuWidgetClass,
                                        menubar, NULL, 0);
    add_menu_entry(go_menu, "Back", go_back_cb, fm);
    add_menu_entry(go_menu, "Forward", go_fwd_cb, fm);
    add_menu_entry(go_menu, "Up", go_up_cb, fm);
    add_separator(go_menu);
    add_menu_entry(go_menu, "Home", go_home_cb, fm);
    add_menu_entry(go_menu, "Root (/)", go_root_cb, fm);
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

    /* MainWindow provides menubar + content area */
    fm->main_window = XtCreateManagedWidget("mainWin", mainWindowWidgetClass,
                                            fm->toplevel, NULL, 0);

    Widget menubar = IswMainWindowGetMenuBar(fm->main_window);
    setup_menus(fm, menubar);

    /* Single content container: nav bar on top, file view below */
    Arg fargs[2];
    Cardinal fn = 0;
    XtSetArg(fargs[fn], XtNdefaultDistance, 0); fn++;
    XtSetArg(fargs[fn], XtNborderWidth, 0);    fn++;
    fm->paned = XtCreateManagedWidget("content", formWidgetClass,
                                      fm->main_window, fargs, fn);

    /* Navigation bar (full width, top) */
    navbar_init(fm);

    /* Places sidebar (left side, below navbar) */
    icons_init();
    places_init(fm);

    /* File view (right of sidebar, below navbar) */
    fileview_init(fm);

    /* Enable XDND and clipboard */
    ISWXdndEnable(fm->toplevel);
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
    const char *home = getenv("HOME");
    fm->cwd = strdup(home ? home : "/");
    fm->history[0] = strdup(fm->cwd);
    fm->hist_pos = 0;
    fm->hist_count = 1;

    browser_read_dir(fm, fm->cwd);

    XtRealizeWidget(fm->toplevel);

    /* After realization, size viewports to fill the Form.
     * Form chain constraints handle resize, but initial layout
     * is based on children's preferred sizes which are too small. */
    {
        Dimension form_w, form_h, nav_h;
        Arg qa[2];
        XtSetArg(qa[0], XtNwidth, &form_w);
        XtSetArg(qa[1], XtNheight, &form_h);
        XtGetValues(fm->paned, qa, 2);
        XtSetArg(qa[0], XtNheight, &nav_h);
        XtGetValues(fm->nav_box, qa, 1);

        Dimension vp_h = form_h > nav_h ? form_h - nav_h : 1;
        Dimension places_w = isde_scale(160);
        Dimension file_w = form_w > places_w ? form_w - places_w : 1;

        XtConfigureWidget(fm->nav_box, 0, 0, form_w, nav_h, 0);
        XtConfigureWidget(fm->places_vp, 0, nav_h, places_w, vp_h, 0);
        XtConfigureWidget(fm->viewport, places_w, nav_h, file_w, vp_h, 0);
    }

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
    clipboard_cleanup(fm);
    browser_free_entries(fm);
    fileview_cleanup(fm);
    places_cleanup(fm);
    icons_cleanup();
    free(fm->cwd);
    for (int i = 0; i < fm->hist_count; i++)
        free(fm->history[i]);
    XtDestroyApplicationContext(fm->app);
}
