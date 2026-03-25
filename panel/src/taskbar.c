#define _POSIX_C_SOURCE 200809L
/*
 * taskbar.c — grouped taskbar (Windows 7 style)
 *
 * Groups windows by WM_CLASS. One button per application:
 *   - 0 windows + pinned: click launches the app
 *   - 1 window: click focuses/raises it
 *   - >1 windows: click shows a popup menu listing all windows
 *   - Right-click: pin/unpin context menu
 */
#include "panel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* ---------- helpers ---------- */

static char *get_wm_class(Panel *p, xcb_window_t win)
{
    char *instance = NULL, *class = NULL;
    if (isde_ewmh_get_wm_class(p->ewmh, win, &instance, &class)) {
        free(instance);
        return class;
    }
    return strdup("unknown");
}

static char *get_window_title(Panel *p, xcb_window_t win)
{
    xcb_get_property_reply_t *reply = xcb_get_property_reply(
        p->conn,
        xcb_get_property(p->conn, 0, win, p->atom_net_wm_name,
                         XCB_ATOM_ANY, 0, 256),
        NULL);
    if (reply && reply->value_len > 0) {
        char *title = strndup(xcb_get_property_value(reply),
                              reply->value_len);
        free(reply);
        return title;
    }
    free(reply);

    reply = xcb_get_property_reply(
        p->conn,
        xcb_get_property(p->conn, 0, win, p->atom_wm_name,
                         XCB_ATOM_ANY, 0, 256),
        NULL);
    if (reply && reply->value_len > 0) {
        char *title = strndup(xcb_get_property_value(reply),
                              reply->value_len);
        free(reply);
        return title;
    }
    free(reply);
    return strdup("(untitled)");
}

/* ---------- callbacks ---------- */

typedef struct {
    Panel     *panel;
    TaskGroup *group;
} TaskClosure;

typedef struct {
    Panel        *panel;
    xcb_window_t  window;
} WindowClosure;

static void focus_window(Panel *p, xcb_window_t win)
{
    /* Send _NET_ACTIVE_WINDOW — the WM should handle raising
     * and unmapping minimized windows */
    isde_ewmh_request_active_window(p->ewmh, win);
}

static void launch_app(Panel *p, TaskGroup *g)
{
    if (g->desktop_exec) {
        pid_t pid = fork();
        if (pid == 0) {
            execl("/bin/sh", "sh", "-c", g->desktop_exec, (char *)NULL);
            _exit(127);
        }
    }
}

static void window_menu_callback(Widget w, XtPointer client_data,
                                 XtPointer call_data)
{
    (void)w;
    (void)call_data;
    WindowClosure *wc = (WindowClosure *)client_data;
    focus_window(wc->panel, wc->window);
}

/* State for the active window list popup */
static Widget   wl_shell = NULL;
static Widget   wl_list  = NULL;
static String  *wl_titles = NULL;
static Panel   *wl_panel  = NULL;
static TaskGroup *wl_group = NULL;

static void wl_select_callback(Widget w, XtPointer client_data,
                                XtPointer call_data)
{
    (void)w;
    (void)client_data;
    IswListReturnStruct *ret = (IswListReturnStruct *)call_data;
    if (!wl_group || ret->list_index < 0 ||
        ret->list_index >= wl_group->nwindows)
        return;

    focus_window(wl_panel, wl_group->windows[ret->list_index]);
    XtPopdown(wl_shell);
}

static void show_window_menu(Panel *p, TaskGroup *g)
{
    /* Destroy previous popup if any */
    if (wl_shell) {
        XtDestroyWidget(wl_shell);
        wl_shell = NULL;
        wl_list = NULL;
    }
    free(wl_titles);
    wl_titles = NULL;

    wl_panel = p;
    wl_group = g;

    /* Build title array — must stay alive while list is shown */
    wl_titles = malloc((g->nwindows + 1) * sizeof(String));
    for (int i = 0; i < g->nwindows; i++)
        wl_titles[i] = get_window_title(p, g->windows[i]);
    wl_titles[g->nwindows] = NULL;

    /* Estimate menu height */
    int menu_h = g->nwindows * 20 + 8;
    if (menu_h > 300) menu_h = 300;

    /* Create popup shell */
    Arg args[8];
    Cardinal n = 0;
    XtSetArg(args[n], XtNwidth, 200);              n++;
    XtSetArg(args[n], XtNheight, menu_h);           n++;
    XtSetArg(args[n], XtNoverrideRedirect, True);   n++;
    XtSetArg(args[n], XtNborderWidth, 1);           n++;
    wl_shell = XtCreatePopupShell("winListMenu", overrideShellWidgetClass,
                                  g->button, args, n);

    /* List widget */
    n = 0;
    XtSetArg(args[n], XtNlist, wl_titles);           n++;
    XtSetArg(args[n], XtNnumberStrings, g->nwindows); n++;
    XtSetArg(args[n], XtNdefaultColumns, 1);          n++;
    XtSetArg(args[n], XtNforceColumns, True);         n++;
    XtSetArg(args[n], XtNverticalList, True);         n++;
    XtSetArg(args[n], XtNborderWidth, 0);             n++;
    XtSetArg(args[n], XtNcursor, None);               n++;
    wl_list = XtCreateManagedWidget("winList", listWidgetClass,
                                    wl_shell, args, n);
    XtAddCallback(wl_list, XtNcallback, wl_select_callback, NULL);

    /* Hover-to-highlight translations */
    static char wlTranslations[] =
        "<EnterWindow>: Set()\n"
        "<LeaveWindow>: Unset()\n"
        "<Motion>:      Set()\n"
        "<BtnDown>:     Set() Notify()\n"
        "<BtnUp>:       Notify()";
    XtOverrideTranslations(wl_list,
                           XtParseTranslationTable(wlTranslations));

    /* Position above the button */
    Position bx, by;
    XtTranslateCoords(g->button, 0, 0, &bx, &by);

    Arg pargs[2];
    XtSetArg(pargs[0], XtNx, bx);
    XtSetArg(pargs[1], XtNy, by - menu_h);
    XtSetValues(wl_shell, pargs, 2);

    XtPopup(wl_shell, XtGrabNone);
}

static void taskbar_button_callback(Widget w, XtPointer client_data,
                                    XtPointer call_data)
{
    (void)w;
    (void)call_data;
    TaskClosure *tc = (TaskClosure *)client_data;
    Panel *p = tc->panel;
    TaskGroup *g = tc->group;

    if (g->nwindows == 0) {
        launch_app(p, g);
    } else if (g->nwindows == 1) {
        focus_window(p, g->windows[0]);
    } else {
        show_window_menu(p, g);
    }
}

/* ---------- right-click context menu (pin/unpin) ---------- */

static void save_pinned(Panel *p)
{
    char *path = isde_xdg_config_path("pinned");
    if (!path) return;

    /* Ensure directory exists */
    char *dir = isde_xdg_config_path("");
    if (dir) {
        mkdir(dir, 0755);
        free(dir);
    }

    FILE *fp = fopen(path, "w");
    if (fp) {
        for (TaskGroup *g = p->groups; g; g = g->next)
            if (g->pinned)
                fprintf(fp, "%s\n", g->wm_class);
        fclose(fp);
    }
    free(path);
}

static void load_pinned_file(Panel *p)
{
    char *path = isde_xdg_config_path("pinned");
    if (!path) return;

    FILE *fp = fopen(path, "r");
    free(path);
    if (!fp) return;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip newline */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (!line[0]) continue;

        p->pinned_classes = realloc(p->pinned_classes,
                                    (p->npinned + 1) * sizeof(char *));
        p->pinned_classes[p->npinned++] = strdup(line);
    }
    fclose(fp);
}

static void pin_callback(Widget w, XtPointer client_data,
                         XtPointer call_data)
{
    (void)w;
    (void)call_data;
    TaskClosure *tc = (TaskClosure *)client_data;
    tc->group->pinned = !tc->group->pinned;
    save_pinned(tc->panel);
}

static void context_menu_handler(Widget w, XtPointer client_data,
                                 XEvent *event, Boolean *cont)
{
    (void)cont;
    if ((event->response_type & ~0x80) != XCB_BUTTON_PRESS)
        return;

    xcb_button_press_event_t *ev = (xcb_button_press_event_t *)event;
    if (ev->detail != 3) /* button 3 = right click */
        return;

    TaskClosure *tc = (TaskClosure *)client_data;
    Panel *p = tc->panel;
    TaskGroup *g = tc->group;

    /* Create context menu */
    Widget ctx = XtCreatePopupShell("ctxMenu", simpleMenuWidgetClass,
                                    w, NULL, 0);

    const char *label = g->pinned ? "Unpin from taskbar"
                                  : "Pin to taskbar";
    Arg args[1];
    XtSetArg(args[0], XtNlabel, label);
    Widget entry = XtCreateManagedWidget("pinToggle", smeBSBObjectClass,
                                         ctx, args, 1);
    XtAddCallback(entry, XtNcallback, pin_callback, client_data);

    Position bx, by;
    XtTranslateCoords(w, 0, 0, &bx, &by);

    if (!XtIsRealized(ctx))
        XtRealizeWidget(ctx);

    Dimension mh;
    Arg qargs[1];
    XtSetArg(qargs[0], XtNheight, &mh);
    XtGetValues(ctx, qargs, 1);

    Arg margs[2];
    XtSetArg(margs[0], XtNx, bx);
    XtSetArg(margs[1], XtNy, by - mh);
    XtSetValues(ctx, margs, 2);

    XtPopup(ctx, XtGrabNone);
}

/* ---------- group management ---------- */

TaskGroup *taskbar_find_group(Panel *p, const char *wm_class)
{
    for (TaskGroup *g = p->groups; g; g = g->next)
        if (g->wm_class && strcmp(g->wm_class, wm_class) == 0)
            return g;
    return NULL;
}

TaskGroup *taskbar_add_group(Panel *p, const char *wm_class)
{
    TaskGroup *g = calloc(1, sizeof(*g));
    g->wm_class = strdup(wm_class);
    g->display_name = strdup(wm_class);
    g->cap_windows = 4;
    g->windows = calloc(g->cap_windows, sizeof(xcb_window_t));

    /* Try to find a matching .desktop entry */
    char cls_lower[128];
    int j;
    for (j = 0; wm_class[j] && j < 126; j++)
        cls_lower[j] = (wm_class[j] >= 'A' && wm_class[j] <= 'Z')
                     ? wm_class[j] + 32 : wm_class[j];
    cls_lower[j] = '\0';

    for (int i = 0; i < p->ndesktop; i++) {
        const char *exec = isde_desktop_exec(p->desktop_entries[i]);
        if (!exec) continue;
        const char *base = strrchr(exec, '/');
        base = base ? base + 1 : exec;

        char exec_lower[128];
        for (j = 0; base[j] && base[j] != ' ' && j < 126; j++)
            exec_lower[j] = (base[j] >= 'A' && base[j] <= 'Z')
                           ? base[j] + 32 : base[j];
        exec_lower[j] = '\0';

        if (strcmp(cls_lower, exec_lower) == 0) {
            const char *name = isde_desktop_name(p->desktop_entries[i]);
            if (name) {
                free(g->display_name);
                g->display_name = strdup(name);
            }
            const char *icon = isde_desktop_icon(p->desktop_entries[i]);
            if (icon) g->desktop_icon = strdup(icon);
            g->desktop_exec = strdup(exec);
            break;
        }
    }

    /* Create button widget */
    Arg args[4];
    Cardinal n = 0;
    XtSetArg(args[n], XtNlabel, g->display_name);   n++;
    XtSetArg(args[n], XtNheight, PANEL_HEIGHT);      n++;
    XtSetArg(args[n], XtNborderWidth, 0);            n++;
    g->button = XtCreateManagedWidget("taskBtn", commandWidgetClass,
                                      p->box, args, n);

    TaskClosure *tc = malloc(sizeof(*tc));
    tc->panel = p;
    tc->group = g;
    XtAddCallback(g->button, XtNcallback, taskbar_button_callback, tc);

    /* Right-click handler for pin/unpin */
    XtAddEventHandler(g->button, ButtonPressMask, False,
                      context_menu_handler, tc);

    /* Link into list */
    g->next = p->groups;
    p->groups = g;

    return g;
}

static void group_add_window(TaskGroup *g, xcb_window_t win)
{
    for (int i = 0; i < g->nwindows; i++)
        if (g->windows[i] == win) return;

    if (g->nwindows >= g->cap_windows) {
        g->cap_windows *= 2;
        g->windows = realloc(g->windows,
                             g->cap_windows * sizeof(xcb_window_t));
    }
    g->windows[g->nwindows++] = win;
}

/* ---------- update from EWMH ---------- */

void taskbar_update(Panel *p)
{
    /* Clear window lists from all groups */
    for (TaskGroup *g = p->groups; g; g = g->next)
        g->nwindows = 0;

    /* Get current client list from WM */
    xcb_window_t *wins = NULL;
    int nwins = isde_ewmh_get_client_list(p->ewmh, &wins);

    for (int i = 0; i < nwins; i++) {
        char *cls = get_wm_class(p, wins[i]);
        TaskGroup *g = taskbar_find_group(p, cls);
        if (!g)
            g = taskbar_add_group(p, cls);
        group_add_window(g, wins[i]);
        free(cls);
    }
    free(wins);

    /* Remove non-pinned groups with 0 windows */
    TaskGroup **pp = &p->groups;
    while (*pp) {
        TaskGroup *g = *pp;
        if (g->nwindows == 0 && !g->pinned) {
            *pp = g->next;
            if (g->button) XtDestroyWidget(g->button);
            if (g->menu) XtDestroyWidget(g->menu);
            free(g->wm_class);
            free(g->display_name);
            free(g->desktop_exec);
            free(g->desktop_icon);
            free(g->windows);
            free(g);
        } else {
            pp = &g->next;
        }
    }
}

/* ---------- init / cleanup ---------- */

void taskbar_init(Panel *p)
{
    /* Load pinned apps from state file */
    load_pinned_file(p);

    for (int i = 0; i < p->npinned; i++) {
        TaskGroup *g = taskbar_add_group(p, p->pinned_classes[i]);
        g->pinned = 1;
    }
}

void taskbar_cleanup(Panel *p)
{
    while (p->groups) {
        TaskGroup *g = p->groups;
        p->groups = g->next;
        free(g->wm_class);
        free(g->display_name);
        free(g->desktop_exec);
        free(g->desktop_icon);
        free(g->windows);
        free(g);
    }
}
