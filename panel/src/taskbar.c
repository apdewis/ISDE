#define _POSIX_C_SOURCE 200809L
/*
 * taskbar.c — grouped taskbar (Windows 7 style)
 *
 * Groups windows by WM_CLASS. One button per application:
 *   - 0 windows + pinned: click launches the app
 *   - 1 window: click focuses it
 *   - >1 windows: click shows a popup menu listing all windows
 */
#include "panel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------- helpers ---------- */

static char *get_wm_class(Panel *p, xcb_window_t win)
{
    xcb_icccm_get_wm_class_reply_t reply;
    if (!xcb_icccm_get_wm_class_reply(
            p->conn,
            xcb_icccm_get_wm_class(p->conn, win),
            &reply, NULL))
        return strdup("unknown");
    char *cls = strdup(reply.class_name);
    xcb_icccm_get_wm_class_reply_wipe(&reply);
    return cls;
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

/* Closure passed to taskbar button callbacks */
typedef struct {
    Panel     *panel;
    TaskGroup *group;
} TaskClosure;

/* Closure for individual window menu entries */
typedef struct {
    Panel        *panel;
    xcb_window_t  window;
} WindowClosure;

static void focus_window(Panel *p, xcb_window_t win)
{
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

static void taskbar_button_callback(Widget w, XtPointer client_data,
                                    XtPointer call_data)
{
    (void)w;
    (void)call_data;
    TaskClosure *tc = (TaskClosure *)client_data;
    Panel *p = tc->panel;
    TaskGroup *g = tc->group;

    if (g->nwindows == 0) {
        /* No instances — launch the app */
        launch_app(p, g);
    } else if (g->nwindows == 1) {
        /* Single instance — focus it */
        focus_window(p, g->windows[0]);
    } else {
        /* Multiple instances — show popup menu */
        if (g->menu)
            XtDestroyWidget(g->menu);

        g->menu = XtCreatePopupShell("windowMenu", simpleMenuWidgetClass,
                                     g->button, NULL, 0);

        for (int i = 0; i < g->nwindows; i++) {
            char *title = get_window_title(p, g->windows[i]);
            Arg args[1];
            XtSetArg(args[0], XtNlabel, title);
            Widget entry = XtCreateManagedWidget("winEntry",
                                                  smeBSBObjectClass,
                                                  g->menu, args, 1);
            WindowClosure *wc = malloc(sizeof(*wc));
            wc->panel = p;
            wc->window = g->windows[i];
            XtAddCallback(entry, XtNcallback, window_menu_callback, wc);
            free(title);
        }

        /* Position menu above the button */
        Arg margs[2];
        Cardinal mn = 0;
        Dimension bw, bh;
        Position bx, by;
        XtSetArg(margs[0], XtNwidth, &bw);
        XtSetArg(margs[1], XtNheight, &bh);
        XtGetValues(g->button, margs, 2);
        XtTranslateCoords(g->button, 0, 0, &bx, &by);

        mn = 0;
        XtSetArg(margs[mn], XtNx, bx);   mn++;
        XtSetArg(margs[mn], XtNy, by - 1); mn++;  /* just above panel */
        XtSetValues(g->menu, margs, mn);

        XtPopup(g->menu, XtGrabExclusive);
    }
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
    g->display_name = strdup(wm_class); /* default display name */
    g->cap_windows = 4;
    g->windows = calloc(g->cap_windows, sizeof(xcb_window_t));

    /* Try to find a matching .desktop entry */
    for (int i = 0; i < p->ndesktop; i++) {
        const char *exec = isde_desktop_exec(p->desktop_entries[i]);
        if (!exec) continue;
        const char *base = strrchr(exec, '/');
        base = base ? base + 1 : exec;

        /* Simple match: lowercase class vs lowercase exec basename */
        char cls_lower[128], exec_lower[128];
        int j;
        for (j = 0; wm_class[j] && j < 126; j++)
            cls_lower[j] = (wm_class[j] >= 'A' && wm_class[j] <= 'Z')
                         ? wm_class[j] + 32 : wm_class[j];
        cls_lower[j] = '\0';
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

    /* Link into list */
    g->next = p->groups;
    p->groups = g;

    return g;
}

static void group_add_window(TaskGroup *g, xcb_window_t win)
{
    /* Check if already present */
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

    /* Remove non-pinned groups with 0 windows,
     * hide buttons for empty pinned groups (but keep them) */
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
    /* Create pinned groups (visible even with 0 windows) */
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
        /* widgets destroyed with shell */
        free(g);
    }
}
