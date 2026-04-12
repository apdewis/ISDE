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

/* Strip desktop field codes (%f, %F, %u, %U, etc.) from an Exec string.
 * Returns a malloc'd copy with codes removed. */
static char *strip_field_codes(const char *src)
{
    size_t len = strlen(src);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t pos = 0;
    while (*src) {
        if (*src == '%' && src[1]) {
            src += 2; /* skip %x */
        } else {
            out[pos++] = *src++;
        }
    }
    /* Trim trailing whitespace left by removed codes */
    while (pos > 0 && out[pos - 1] == ' ') pos--;
    out[pos] = '\0';
    return out;
}

/* ---------- helpers ---------- */

static char *get_window_title(Panel *p, xcb_window_t win);
static Pixel taskbar_pixel(Panel *p, unsigned int rgb);

static char *get_wm_class(Panel *p, xcb_window_t win)
{
    char *instance = NULL, *class = NULL;
    if (isde_ewmh_get_wm_class(p->ewmh, win, &instance, &class)) {
        if (class && *class) {
            free(instance);
            return class;
        }
        /* Class empty but instance available */
        free(class);
        if (instance && *instance) {
            return instance;
        }
        free(instance);
    }

    /* Last resort: use the window title */
    char *title = get_window_title(p, win);
    return title;
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
        ret->list_index >= wl_group->nwindows) {
        return;
    }

    focus_window(wl_panel, wl_group->windows[ret->list_index]);
    panel_dismiss_popup(wl_panel);
}

static void wl_motion_handler(Widget w, XtPointer client_data,
                              xcb_generic_event_t *event, Boolean *cont)
{
    (void)client_data;
    (void)cont;
    xcb_motion_notify_event_t *ev = (xcb_motion_notify_event_t *)event;
    /* Use the List's Set action to highlight the item under the pointer.
     * Set() reads event_x/event_y which are at the same struct offset
     * for both button_press and motion_notify events. */
    XtCallActionProc(w, "Set", event, NULL, 0);
    (void)ev;
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
    for (int i = 0; i < g->nwindows; i++) {
        wl_titles[i] = get_window_title(p, g->windows[i]);
    }
    wl_titles[g->nwindows] = NULL;

    /* Create popup shell */
    const IsdeColorScheme *s = isde_theme_current();
    Pixel border_px = s ? taskbar_pixel(p, s->border)
                        : p->screen->white_pixel;

    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNoverrideRedirect, True);    n++;
    XtSetArg(args[n], XtNborderWidth, 1);            n++;
    XtSetArg(args[n], XtNborderColor, border_px);    n++;
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

    /* Hover-to-highlight + click-to-select translations */
    static char wlTranslations[] =
        "<EnterWindow>: Set()\n"
        "<LeaveWindow>: Unset()\n"
        "<Btn1Motion>:  Set()\n"
        "<BtnDown>:     Set() Notify()\n"
        "<BtnUp>:       Notify()";
    XtOverrideTranslations(wl_list,
                           XtParseTranslationTable(wlTranslations));

    /* Motion handler for hover highlight (translations may not get
     * XCB_EVENT_MASK_POINTER_MOTION added when set via XtOverrideTranslations) */
    XtAddEventHandler(wl_list, XCB_EVENT_MASK_POINTER_MOTION, False,
                      wl_motion_handler, NULL);

    /* Realize to get actual size, then position bottom-flush with panel */
    if (!XtIsRealized(wl_shell)) {
        XtRealizeWidget(wl_shell);
    }

    /* Query the list's actual size and resize the shell to fit */
    Dimension list_w, list_h;
    n = 0;
    XtSetArg(args[n], XtNwidth, &list_w);   n++;
    XtSetArg(args[n], XtNheight, &list_h);  n++;
    XtGetValues(wl_list, args, n);

    Dimension bw;
    n = 0;
    XtSetArg(args[n], XtNborderWidth, &bw); n++;
    XtGetValues(wl_shell, args, n);

    Position bx, by;
    XtTranslateCoords(g->button, 0, 0, &bx, &by);

    XtConfigureWidget(wl_shell, bx,
                      by - (Position)list_h - (Position)(2 * bw),
                      list_w, list_h, bw);

    XtPopup(wl_shell, XtGrabNone);
    panel_show_popup(p, wl_shell);
}

static void taskbar_button_callback(Widget w, XtPointer client_data,
                                    XtPointer call_data)
{
    (void)w;
    (void)call_data;
    TaskClosure *tc = (TaskClosure *)client_data;
    Panel *p = tc->panel;
    TaskGroup *g = tc->group;

    /* Multi-window case is handled on button press (see below)
     * so the release can land on the popup list item */
    if (g->nwindows == 0) {
        launch_app(p, g);
    } else if (g->nwindows == 1) {
        focus_window(p, g->windows[0]);
    }
}

/* Button-press handler for multi-window popup: showing the window list
 * on press (not release) lets the user drag into the list and release
 * to select, matching standard menu behaviour. */
static void taskbar_press_handler(Widget w, XtPointer client_data,
                                  xcb_generic_event_t *event, Boolean *cont)
{
    (void)w;
    (void)cont;
    if ((event->response_type & ~0x80) != XCB_BUTTON_PRESS) {
        return;
    }
    xcb_button_press_event_t *ev = (xcb_button_press_event_t *)event;
    if (ev->detail != 1) {
        return;
    }

    TaskClosure *tc = (TaskClosure *)client_data;
    Panel *p = tc->panel;
    TaskGroup *g = tc->group;

    if (g->nwindows <= 1) {
        return;  /* handled by the Command callback on release */
    }

    if (p->active_popup && wl_shell && wl_group == g) {
        panel_dismiss_popup(p);
    } else {
        show_window_menu(p, g);
    }
}

/* ---------- right-click context menu (pin/unpin) ---------- */

static void save_pinned(Panel *p)
{
    char *path = isde_xdg_config_path("pinned");
    if (!path) {
        return;
    }

    /* Ensure directory exists */
    char *dir = isde_xdg_config_path("");
    if (dir) {
        mkdir(dir, 0755);
        free(dir);
    }

    FILE *fp = fopen(path, "w");
    if (fp) {
        for (TaskGroup *g = p->groups; g; g = g->next) {
            if (g->pinned) {
                fprintf(fp, "%s\n", g->wm_class);
            }
        }
        fclose(fp);
    }
    free(path);
}

static void load_pinned_file(Panel *p)
{
    char *path = isde_xdg_config_path("pinned");
    if (!path) {
        return;
    }

    FILE *fp = fopen(path, "r");
    free(path);
    if (!fp) {
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip newline */
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        if (!line[0]) {
            continue;
        }

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
    panel_dismiss_popup(tc->panel);
}

typedef struct {
    Panel      *panel;
    char       *exec;  /* expanded command (owned) */
} ActionClosure;

static void action_callback(Widget w, XtPointer client_data,
                            XtPointer call_data)
{
    (void)w;
    (void)call_data;
    ActionClosure *ac = (ActionClosure *)client_data;
    panel_dismiss_popup(ac->panel);
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", ac->exec, (char *)NULL);
        _exit(127);
    }
}

static void close_all_callback(Widget w, XtPointer client_data,
                               XtPointer call_data)
{
    (void)w;
    (void)call_data;
    TaskClosure *tc = (TaskClosure *)client_data;
    Panel *p = tc->panel;
    TaskGroup *g = tc->group;
    panel_dismiss_popup(p);
    for (int i = 0; i < g->nwindows; i++) {
        isde_ewmh_request_close_window(p->ewmh, g->windows[i]);
    }
}

static void context_menu_handler(Widget w, XtPointer client_data,
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

    TaskClosure *tc = (TaskClosure *)client_data;
    Panel *p = tc->panel;
    TaskGroup *g = tc->group;

    /* Toggle: if a popup is already open, just dismiss it */
    if (p->active_popup) {
        panel_dismiss_popup(p);
        return;
    }

    const IsdeColorScheme *s = isde_theme_current();
    Pixel border_px = s ? taskbar_pixel(p, s->border)
                        : p->screen->white_pixel;
    Arg cargs[20];
    Cardinal cn = 0;
    XtSetArg(cargs[cn], XtNborderWidth, 1);          cn++;
    XtSetArg(cargs[cn], XtNborderColor, border_px);  cn++;
    Widget ctx = XtCreatePopupShell("ctxMenu", simpleMenuWidgetClass,
                                    w, cargs, cn);

    Arg args[20];

    /* Desktop actions (e.g. "New Window", "New Private Window") */
    if (g->desktop_index >= 0 && g->desktop_index < p->ndesktop) {
        IsdeDesktopEntry *de = p->desktop_entries[g->desktop_index];
        int nactions = isde_desktop_action_count(de);
        for (int i = 0; i < nactions; i++) {
            const IsdeDesktopAction *a = isde_desktop_action(de, i);
            if (!a->name || !a->exec) {
                continue;
            }
            XtSetArg(args[0], XtNlabel, a->name);
            Widget entry = XtCreateManagedWidget("action", smeBSBObjectClass,
                                                  ctx, args, 1);
            ActionClosure *ac = malloc(sizeof(*ac));
            ac->panel = p;
            ac->exec = strip_field_codes(a->exec);
            XtAddCallback(entry, XtNcallback, action_callback, ac);
        }

        /* Separator if we added actions */
        if (nactions > 0) {
            XtCreateManagedWidget("sep", smeLineObjectClass, ctx, NULL, 0);
        }
    }

    /* New instance */
    if (g->desktop_exec) {
        XtSetArg(args[0], XtNlabel, "New instance");
        Widget ni = XtCreateManagedWidget("newInst", smeBSBObjectClass,
                                           ctx, args, 1);
        ActionClosure *ac = malloc(sizeof(*ac));
        ac->panel = p;
        ac->exec = g->desktop_exec;
        XtAddCallback(ni, XtNcallback, action_callback, ac);
    }

    /* Close all windows */
    if (g->nwindows > 0) {
        XtSetArg(args[0], XtNlabel, "Close all windows");
        Widget entry = XtCreateManagedWidget("closeAll", smeBSBObjectClass,
                                              ctx, args, 1);
        XtAddCallback(entry, XtNcallback, close_all_callback, client_data);

        XtCreateManagedWidget("sep2", smeLineObjectClass, ctx, NULL, 0);
    }

    /* Pin/unpin */
    const char *label = g->pinned ? "Unpin from taskbar"
                                  : "Pin to taskbar";
    XtSetArg(args[0], XtNlabel, label);
    Widget pin_entry = XtCreateManagedWidget("pinToggle", smeBSBObjectClass,
                                              ctx, args, 1);
    XtAddCallback(pin_entry, XtNcallback, pin_callback, client_data);

    /* Position above button, bottom flush with panel top */
    Position bx, by;
    XtTranslateCoords(w, 0, 0, &bx, &by);

    if (!XtIsRealized(ctx)) {
        XtRealizeWidget(ctx);
    }

    Dimension mh, bw;
    Arg qargs[20];
    Cardinal qn = 0;
    XtSetArg(qargs[qn], XtNheight, &mh); qn++;
    XtSetArg(qargs[qn], XtNborderWidth, &bw); qn++;
    XtGetValues(ctx, qargs, qn);

    Arg margs[2];
    XtSetArg(margs[0], XtNx, bx);
    XtSetArg(margs[1], XtNy, by - (Position)mh - (Position)(2 * bw));
    XtSetValues(ctx, margs, 2);

    XtPopup(ctx, XtGrabNone);
    panel_show_popup(p, ctx);
}

/* ---------- group management ---------- */

TaskGroup *taskbar_find_group(Panel *p, const char *wm_class)
{
    for (TaskGroup *g = p->groups; g; g = g->next) {
        if (g->wm_class && strcmp(g->wm_class, wm_class) == 0) {
            return g;
        }
    }
    return NULL;
}

TaskGroup *taskbar_add_group(Panel *p, const char *wm_class)
{
    TaskGroup *g = calloc(1, sizeof(*g));
    g->wm_class = strdup(wm_class);
    g->display_name = strdup(wm_class);
    g->desktop_index = -1;
    g->cap_windows = 4;
    g->windows = calloc(g->cap_windows, sizeof(xcb_window_t));

    /* Try to find a matching .desktop entry.
     * First pass: match by StartupWMClass (the standard mechanism).
     * Second pass: fall back to matching Exec basename. */
    char cls_lower[128];
    int j;
    for (j = 0; wm_class[j] && j < 126; j++) {
        cls_lower[j] = (wm_class[j] >= 'A' && wm_class[j] <= 'Z')
                     ? wm_class[j] + 32 : wm_class[j];
    }
    cls_lower[j] = '\0';

    int match = -1;

    /* Pass 1: StartupWMClass */
    for (int i = 0; i < p->ndesktop && match < 0; i++) {
        const char *swc = isde_desktop_startup_wm_class(p->desktop_entries[i]);
        if (!swc) {
            continue;
        }

        char swc_lower[128];
        for (j = 0; swc[j] && j < 126; j++) {
            swc_lower[j] = (swc[j] >= 'A' && swc[j] <= 'Z')
                          ? swc[j] + 32 : swc[j];
        }
        swc_lower[j] = '\0';

        if (strcmp(cls_lower, swc_lower) == 0) {
            match = i;
        }
    }

    /* Pass 2: Exec basename */
    for (int i = 0; i < p->ndesktop && match < 0; i++) {
        const char *exec = isde_desktop_exec(p->desktop_entries[i]);
        if (!exec) {
            continue;
        }
        const char *base = strrchr(exec, '/');
        base = base ? base + 1 : exec;

        char exec_lower[128];
        for (j = 0; base[j] && base[j] != ' ' && j < 126; j++) {
            exec_lower[j] = (base[j] >= 'A' && base[j] <= 'Z')
                           ? base[j] + 32 : base[j];
        }
        exec_lower[j] = '\0';

        if (strcmp(cls_lower, exec_lower) == 0) {
            match = i;
        }
    }

    if (match >= 0) {
        const char *name = isde_desktop_name(p->desktop_entries[match]);
        if (name) {
            free(g->display_name);
            g->display_name = strdup(name);
        }
        const char *icon = isde_desktop_icon(p->desktop_entries[match]);
        if (icon) {
            g->desktop_icon = strdup(icon);
        }
        g->desktop_exec = isde_desktop_build_exec(
            p->desktop_entries[match], NULL, 0);
        g->desktop_index = match;
    }

    /* Resolve icon: .desktop icon name -> theme lookup -> default fallback */
    if (g->desktop_icon) {
        g->icon_path = isde_icon_find("apps", g->desktop_icon);
    }
    if (!g->icon_path) {
        g->icon_path = isde_icon_find("apps", "application-default");
    }

    /* Create button widget */
    Arg args[20];
    Cardinal n = 0;
    if (g->icon_path) {
        Dimension pad = 2;
        XtSetArg(args[n], XtNimage, g->icon_path);            n++;
        XtSetArg(args[n], XtNlabel, "");                 n++;
        XtSetArg(args[n], XtNwidth, PANEL_HEIGHT);       n++;
        XtSetArg(args[n], XtNinternalWidth, pad);        n++;
        XtSetArg(args[n], XtNinternalHeight, pad);       n++;
    } else {
        XtSetArg(args[n], XtNlabel, g->display_name);   n++;
    }
    XtSetArg(args[n], XtNheight, PANEL_HEIGHT);        n++;
    XtSetArg(args[n], XtNborderWidth, 0);              n++;
    g->button = XtCreateManagedWidget("taskBtn", commandWidgetClass,
                                      p->box, args, n);

    TaskClosure *tc = malloc(sizeof(*tc));
    tc->panel = p;
    tc->group = g;
    XtAddCallback(g->button, XtNcallback, taskbar_button_callback, tc);

    /* Button-press handler: left-click shows window list for multi-window
     * groups; right-click opens pin/unpin context menu */
    XtAddEventHandler(g->button, XCB_EVENT_MASK_BUTTON_PRESS, False,
                      taskbar_press_handler, tc);
    XtAddEventHandler(g->button, XCB_EVENT_MASK_BUTTON_PRESS, False,
                      context_menu_handler, tc);

    /* Link into list */
    g->next = p->groups;
    p->groups = g;

    return g;
}

static void group_add_window(TaskGroup *g, xcb_window_t win)
{
    for (int i = 0; i < g->nwindows; i++) {
        if (g->windows[i] == win) {
            return;
        }
    }

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
    for (TaskGroup *g = p->groups; g; g = g->next) {
        g->nwindows = 0;
    }

    /* Get current client list from WM */
    xcb_window_t *wins = NULL;
    int nwins = isde_ewmh_get_client_list(p->ewmh, &wins);

    for (int i = 0; i < nwins; i++) {
        char *cls = get_wm_class(p, wins[i]);
        TaskGroup *g = taskbar_find_group(p, cls);
        if (!g) {
            g = taskbar_add_group(p, cls);
        }
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
            if (g->button) {
                XtDestroyWidget(g->button);
            }
            if (g->menu) {
                XtDestroyWidget(g->menu);
            }
            free(g->wm_class);
            free(g->display_name);
            free(g->desktop_exec);
            free(g->desktop_icon);
            free(g->icon_path);
            free(g->windows);
            free(g);
        } else {
            pp = &g->next;
        }
    }
}

/* ---------- active window highlight ---------- */

static Pixel taskbar_pixel(Panel *p, unsigned int rgb)
{
    xcb_alloc_color_reply_t *reply = xcb_alloc_color_reply(
        p->conn,
        xcb_alloc_color(p->conn, p->screen->default_colormap,
                        ((rgb >> 16) & 0xFF) * 257,
                        ((rgb >> 8)  & 0xFF) * 257,
                        ( rgb        & 0xFF) * 257),
        NULL);
    if (!reply) {
        return p->screen->white_pixel;
    }
    Pixel px = reply->pixel;
    free(reply);
    return px;
}

void taskbar_highlight_active(Panel *p)
{
    const IsdeColorScheme *s = isde_theme_current();
    if (!s) {
        return;
    }

    xcb_window_t active = isde_ewmh_get_active_window(p->ewmh);
    char *active_class = NULL;
    if (active != XCB_WINDOW_NONE) {
        active_class = get_wm_class(p, active);
    }

    for (TaskGroup *g = p->groups; g; g = g->next) {
        if (!g->button) {
            continue;
        }

        int is_focused = (active_class && g->wm_class &&
                          strcmp(g->wm_class, active_class) == 0);

        const IsdeElementColors *ec;
        if (is_focused) {
            ec = &s->taskbar_button_focus;
        } else if (g->nwindows > 0) {
            ec = &s->taskbar_button_active;
        } else {
            ec = &s->taskbar_button;
        }

        Arg args[20];
        Cardinal n = 0;
        XtSetArg(args[n], XtNbackground, taskbar_pixel(p, ec->bg)); n++;
        XtSetArg(args[n], XtNforeground, taskbar_pixel(p, ec->fg)); n++;
        XtSetValues(g->button, args, n);
    }

    free(active_class);
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
        free(g->icon_path);
        free(g->windows);
        free(g);
    }
}
