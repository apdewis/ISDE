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

static void delete_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    /* TODO: delete selected entries */
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
    /* Copy path first — it may point into fm->entries which
     * browser_read_dir will free */
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
    /* Clear the view before freeing entries — the IconView holds
     * pointers to the old label/icon strings */
    if (fm->iconview)
        IswIconViewSetItems(fm->iconview, NULL, NULL, 0);

    browser_read_dir(fm, fm->cwd);
    fileview_populate(fm);
    navbar_update(fm);
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

    fm->toplevel = XtAppInitialize(&fm->app, "ISDE-FM",
                                   NULL, 0, argc, argv,
                                   NULL, NULL, 0);

    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNwidth, 700);  n++;
    XtSetArg(args[n], XtNheight, 500); n++;
    XtSetValues(fm->toplevel, args, n);

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

    /* Navigation bar (inside paned) */
    navbar_init(fm);

    /* File view (inside paned) */
    icons_init();
    fileview_init(fm);

    /* Enable XDND and clipboard */
    ISWXdndEnable(fm->toplevel);
    clipboard_init(fm);

    /* Start in home directory */
    const char *home = getenv("HOME");
    fm->cwd = strdup(home ? home : "/");
    fm->history[0] = strdup(fm->cwd);
    fm->hist_pos = 0;
    fm->hist_count = 1;

    browser_read_dir(fm, fm->cwd);

    XtRealizeWidget(fm->toplevel);

    fileview_populate(fm);
    navbar_update(fm);

    fm->running = 1;
    return 0;
}

void fm_run(Fm *fm)
{
    while (fm->running) {
        XtAppProcessEvent(fm->app, XtIMAll);
    }
}

void fm_cleanup(Fm *fm)
{
    clipboard_cleanup(fm);
    browser_free_entries(fm);
    fileview_cleanup(fm);
    icons_cleanup();
    free(fm->cwd);
    for (int i = 0; i < fm->hist_count; i++)
        free(fm->history[i]);
    XtDestroyApplicationContext(fm->app);
}
