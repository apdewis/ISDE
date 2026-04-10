#define _POSIX_C_SOURCE 200809L
/*
 * startmenu.c — custom two-pane start menu
 *
 * OverrideShell with two List widgets:
 *   Left:  category names — click to populate right pane
 *   Right: app names for selected category — click to launch
 */
#include "panel.h"
#include <X11/ShellP.h>
#include <ISW/List.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xcb/xcb_keysyms.h>
#include <X11/keysym.h>

static char *start_icon_path;
static char *shutdown_icon_path;
static char *reboot_icon_path;
static char *logout_icon_path;

static Pixel start_color_pixel(Panel *p, unsigned int rgb)
{
    xcb_connection_t *conn = XtDisplay(p->start_btn);
    xcb_screen_t *screen = XtScreen(p->start_btn);
    xcb_alloc_color_reply_t *reply = xcb_alloc_color_reply(
        conn,
        xcb_alloc_color(conn, screen->default_colormap,
                        ((rgb >> 16) & 0xFF) * 257,
                        ((rgb >> 8)  & 0xFF) * 257,
                        ( rgb        & 0xFF) * 257),
        NULL);
    if (!reply) {
        return screen->white_pixel;
    }
    Pixel px = reply->pixel;
    free(reply);
    return px;
}

/* State-dependent: active inverts to show the menu is open */
static void set_start_btn_active(Panel *p, int active)
{
    const IsdeColorScheme *s = isde_theme_current();
    if (!s) {
        return;
    }
    Arg args[2];
    if (active) {
        XtSetArg(args[0], XtNforeground,
                 start_color_pixel(p, s->taskbar_button.hover_fg));
        XtSetArg(args[1], XtNbackground,
                 start_color_pixel(p, s->active));
    } else {
        XtSetArg(args[0], XtNforeground,
                 start_color_pixel(p, s->taskbar_button.fg));
        XtSetArg(args[1], XtNbackground,
                 start_color_pixel(p, s->taskbar_button.bg));
    }
    XtSetValues(p->start_btn, args, 2);
}

#define MENU_WIDTH       400
#define MENU_HEIGHT      350
#define CAT_PANE_WIDTH   150
#define TOOLBAR_HEIGHT   28

/* Standard freedesktop.org category mapping */
static const struct { const char *key; const char *label; } CAT_MAP[] = {
    { "AudioVideo",  "Multimedia" },
    { "Audio",       "Multimedia" },
    { "Video",       "Multimedia" },
    { "Development", "Development" },
    { "Education",   "Education" },
    { "Game",        "Games" },
    { "Graphics",    "Graphics" },
    { "Network",     "Internet" },
    { "Office",      "Office" },
    { "Settings",    "Settings" },
    { "System",      "System" },
    { "Utility",     "Accessories" },
};
#define NUM_CAT_MAP (sizeof(CAT_MAP) / sizeof(CAT_MAP[0]))

static const char *map_category(const char *categories)
{
    if (!categories) {
        return "Other";
    }
    for (int i = 0; i < (int)NUM_CAT_MAP; i++) {
        const char *cat = CAT_MAP[i].key;
        size_t clen = strlen(cat);
        const char *p = categories;
        while (*p) {
            const char *semi = strchr(p, ';');
            size_t elen = semi ? (size_t)(semi - p) : strlen(p);
            if (elen == clen && strncmp(p, cat, clen) == 0) {
                return CAT_MAP[i].label;
            }
            p = semi ? semi + 1 : p + elen;
        }
    }
    return "Other";
}

/* ---------- build category data ---------- */

static StartMenuCategory *find_or_add_cat(Panel *p, const char *label)
{
    for (int i = 0; i < p->ncategories; i++) {
        if (strcmp(p->categories[i].label, label) == 0) {
            return &p->categories[i];
        }
    }

    p->categories = realloc(p->categories,
                            (p->ncategories + 1) * sizeof(StartMenuCategory));
    StartMenuCategory *c = &p->categories[p->ncategories++];
    memset(c, 0, sizeof(*c));
    c->label = label;
    c->cap = 16;
    c->apps = calloc(c->cap, sizeof(StartMenuApp));
    return c;
}

static void cat_add_app(StartMenuCategory *c, const char *name,
                        IsdeDesktopEntry *entry, const char *icon)
{
    if (c->napps >= c->cap) {
        c->cap *= 2;
        c->apps = realloc(c->apps, c->cap * sizeof(StartMenuApp));
    }
    c->apps[c->napps].name = name;
    c->apps[c->napps].entry = entry;
    c->apps[c->napps].icon = icon;
    c->napps++;
}

static void build_categories(Panel *p)
{
    for (int i = 0; i < p->ndesktop; i++) {
        IsdeDesktopEntry *de = p->desktop_entries[i];
        if (!isde_desktop_should_show(de, "ISDE")) {
            continue;
        }
        if (isde_desktop_no_display(de) || isde_desktop_hidden(de)) {
            continue;
        }
        const char *name = isde_desktop_name(de);
        if (!isde_desktop_exec(de) || !name) {
            continue;
        }

        const char *label = map_category(isde_desktop_categories(de));
        StartMenuCategory *c = find_or_add_cat(p, label);
        cat_add_app(c, name, de, isde_desktop_icon(de));
    }
}

/* ---------- show apps for selected category ---------- */

static void show_category(Panel *p, int index)
{
    if (index < 0 || index >= p->ncategories) {
        return;
    }
    p->active_cat = index;

    StartMenuCategory *c = &p->categories[index];

    /* Build string list for the app List widget */
    String *names = malloc((c->napps + 1) * sizeof(String));
    for (int i = 0; i < c->napps; i++) {
        names[i] = (String)c->apps[i].name;
    }
    names[c->napps] = NULL;

    IswListChange(p->app_box, names, c->napps, 0, True);
    IswViewportSetLocation(p->app_viewport, 0.0, 0.0);
    XtMapWidget(p->app_viewport);
    /* Don't free names — the List widget holds the pointer.
     * Previous array leaks, but it's small and infrequent. */
}

static void launch_app(Panel *p, int index)
{
    if (p->active_cat < 0 || p->active_cat >= p->ncategories) {
        return;
    }
    StartMenuCategory *c = &p->categories[p->active_cat];
    if (index < 0 || index >= c->napps) {
        return;
    }

    IsdeDesktopEntry *de = c->apps[index].entry;
    panel_dismiss_popup(p);

    char *cmd = isde_desktop_build_exec(de, NULL, 0);
    if (cmd) {
        pid_t pid = fork();
        if (pid == 0) {
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        free(cmd);
    }
}

/* ---------- callbacks ---------- */

static void category_selected(Widget w, XtPointer client_data,
                              XtPointer call_data)
{
    (void)w;
    Panel *p = (Panel *)client_data;
    IswListReturnStruct *ret = (IswListReturnStruct *)call_data;
    if (ret->list_index == XAW_LIST_NONE) {
        return;
    }
    show_category(p, ret->list_index);
}

static void app_selected(Widget w, XtPointer client_data,
                         XtPointer call_data)
{
    (void)w;
    Panel *p = (Panel *)client_data;
    IswListReturnStruct *ret = (IswListReturnStruct *)call_data;
    if (ret->list_index == XAW_LIST_NONE) {
        return;
    }
    launch_app(p, ret->list_index);
}

/* ---------- keyboard navigation ---------- */

static xcb_key_symbols_t *key_syms;

static void menu_key_handler(Widget w, XtPointer client_data,
                             xcb_generic_event_t *xev, Boolean *cont)
{
    (void)w; (void)cont;
    Panel *p = (Panel *)client_data;
    if ((xev->response_type & ~0x80) != XCB_KEY_PRESS) {
        return;
    }

    xcb_key_press_event_t *kev = (xcb_key_press_event_t *)xev;

    if (!key_syms) {
        key_syms = xcb_key_symbols_alloc(p->conn);
    }

    xcb_keysym_t sym = xcb_key_symbols_get_keysym(key_syms,
                                                   kev->detail, 0);

    switch (sym) {
    case XK_Down:
        if (p->menu_focus == 0) {
            int next = p->cat_highlight + 1;
            if (next >= p->ncategories) {
                next = p->ncategories - 1;
            }
            p->cat_highlight = next;
            IswListHighlight(p->cat_box, next);
            show_category(p, next);
            p->app_highlight = -1;
            IswListUnhighlight(p->app_box);
        } else {
            if (p->active_cat < 0) {
                break;
            }
            StartMenuCategory *c = &p->categories[p->active_cat];
            int next = p->app_highlight + 1;
            if (next >= c->napps) {
                next = c->napps - 1;
            }
            p->app_highlight = next;
            IswListHighlight(p->app_box, next);
        }
        break;

    case XK_Up:
        if (p->menu_focus == 0) {
            int next = p->cat_highlight - 1;
            if (next < 0) {
                next = 0;
            }
            p->cat_highlight = next;
            IswListHighlight(p->cat_box, next);
            show_category(p, next);
            p->app_highlight = -1;
            IswListUnhighlight(p->app_box);
        } else {
            int next = p->app_highlight - 1;
            if (next < 0) {
                next = 0;
            }
            p->app_highlight = next;
            IswListHighlight(p->app_box, next);
        }
        break;

    case XK_Right:
        if (p->menu_focus == 1 || p->active_cat < 0) {
            break;
        }
        p->menu_focus = 1;
        p->app_highlight = 0;
        IswListHighlight(p->app_box, 0);
        break;

    case XK_Left:
        if (p->menu_focus == 0) {
            break;
        }
        p->menu_focus = 0;
        p->app_highlight = -1;
        IswListUnhighlight(p->app_box);
        break;

    case XK_Return:
    case XK_KP_Enter:
        if (p->menu_focus == 0) {
            if (p->active_cat < 0) {
                break;
            }
            p->menu_focus = 1;
            p->app_highlight = 0;
            IswListHighlight(p->app_box, 0);
        } else {
            launch_app(p, p->app_highlight);
        }
        break;

    case XK_Escape:
        panel_dismiss_popup(p);
        break;
    }
}

static void shutdown_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w; (void)call_data;
    Panel *p = (Panel *)client_data;
    panel_dismiss_popup(p);
    isde_ipc_send(p->ipc, ISDE_CMD_SHUTDOWN, 0, 0, 0, 0);
}

static void reboot_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w; (void)call_data;
    Panel *p = (Panel *)client_data;
    panel_dismiss_popup(p);
    isde_ipc_send(p->ipc, ISDE_CMD_REBOOT, 0, 0, 0, 0);
}

static void logout_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w; (void)call_data;
    Panel *p = (Panel *)client_data;
    panel_dismiss_popup(p);
    isde_ipc_send(p->ipc, ISDE_CMD_LOGOUT, 0, 0, 0, 0);
}

/* ---------- toggle ---------- */

static void toggle_start_menu(Widget w, XtPointer client_data,
                              XtPointer call_data)
{
    (void)w;
    (void)call_data;
    Panel *p = (Panel *)client_data;

    if (p->active_popup == p->start_shell) {
        panel_dismiss_popup(p);
        return;
    }
    set_start_btn_active(p, 1);

    /* Position above the panel at the left edge of primary monitor.
     * Use physical dimensions from the realized widgets. */
    int phys_panel_h = p->shell->core.height;
    int panel_y = p->mon_y + p->mon_h - phys_panel_h;

    if (!XtIsRealized(p->start_shell)) {
        XtRealizeWidget(p->start_shell);
    }
    int menu_w = p->start_shell->core.width;
    int menu_h = p->start_shell->core.height;
    int menu_y = panel_y - menu_h;
    XtConfigureWidget(p->start_shell, p->mon_x, menu_y,
                      menu_w, menu_h, 1);
    XtPopup(p->start_shell, XtGrabNone);
    panel_show_popup(p, p->start_shell);
    p->active_cat = -1;
    p->cat_highlight = 0;
    p->app_highlight = -1;
    p->menu_focus = 0;
    XtUnmapWidget(p->app_viewport);

    /* Highlight first category and grab keyboard */
    IswListHighlight(p->cat_box, 0);
    show_category(p, 0);
    xcb_grab_keyboard(p->conn, 1, XtWindow(p->start_shell),
                      XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC,
                      XCB_GRAB_MODE_ASYNC);
    xcb_flush(p->conn);
}

/* ---------- init / cleanup ---------- */

void startmenu_init(Panel *p)
{
    build_categories(p);
    p->active_cat = -1;
    p->cat_highlight = -1;
    p->app_highlight = -1;
    p->menu_focus = 0;

    /* Resolve start menu and logout icons from theme */
    free(start_icon_path);
    start_icon_path = isde_icon_find("actions", "application-menu");

    free(shutdown_icon_path);
    shutdown_icon_path = isde_icon_find("actions", "system-shutdown");

    free(reboot_icon_path);
    reboot_icon_path = isde_icon_find("actions", "system-reboot");

    free(logout_icon_path);
    logout_icon_path = isde_icon_find("actions", "system-log-out");

    /* Start button — child of form, pinned left */
    Arg args[20];
    Cardinal n = 0;
    if (start_icon_path) {
        XtSetArg(args[n], XtNimage, start_icon_path);       n++;
    }
    XtSetArg(args[n], XtNlabel, "");                 n++;
    XtSetArg(args[n], XtNwidth, PANEL_HEIGHT);       n++;
    XtSetArg(args[n], XtNheight, PANEL_HEIGHT);      n++;
    XtSetArg(args[n], XtNborderWidth, 0);            n++;
    XtSetArg(args[n], XtNleft, XtChainLeft);         n++;
    XtSetArg(args[n], XtNright, XtChainLeft);        n++;
    XtSetArg(args[n], XtNtop, XtChainTop);           n++;
    XtSetArg(args[n], XtNbottom, XtChainBottom);     n++;
    p->start_btn = XtCreateManagedWidget("startBtn", commandWidgetClass,
                                         p->form, args, n);
    XtAddCallback(p->start_btn, XtNcallback, toggle_start_menu, p);

    /* Start menu shell */
    n = 0;
    const IsdeColorScheme *scheme_border = isde_theme_current();
    Pixel border_px = scheme_border
        ? start_color_pixel(p, scheme_border->border)
        : XtScreen(p->start_btn)->white_pixel;
    XtSetArg(args[n], XtNwidth, MENU_WIDTH);          n++;
    XtSetArg(args[n], XtNheight, MENU_HEIGHT);         n++;
    XtSetArg(args[n], XtNoverrideRedirect, True);      n++;
    XtSetArg(args[n], XtNborderWidth, 1);              n++;
    XtSetArg(args[n], XtNborderColor, border_px);      n++;
    p->start_shell = XtCreatePopupShell("startMenu",
                                        overrideShellWidgetClass,
                                        p->start_btn, args, n);

    /* Form container — single child of the shell */
    n = 0;
    XtSetArg(args[n], XtNdefaultDistance, 0);  n++;
    XtSetArg(args[n], XtNborderWidth, 0);      n++;
    Widget form = XtCreateManagedWidget("menuForm", formWidgetClass,
                                        p->start_shell, args, n);

    /* Pane background tones from theme */
    const IsdeColorScheme *scheme = isde_theme_current();
    Pixel cat_bg  = scheme ? start_color_pixel(p, scheme->bg)
                           : XtScreen(p->start_btn)->white_pixel;
    Pixel app_bg  = scheme ? start_color_pixel(p, scheme->bg_light)
                           : XtScreen(p->start_btn)->white_pixel;

    /* Category list (left pane) — darker tone */
    String *cat_names = malloc((p->ncategories + 1) * sizeof(String));
    for (int i = 0; i < p->ncategories; i++) {
        cat_names[i] = (String)p->categories[i].label;
    }
    cat_names[p->ncategories] = NULL;

    n = 0;
    XtSetArg(args[n], XtNlist, cat_names);                    n++;
    XtSetArg(args[n], XtNnumberStrings, p->ncategories);     n++;
    XtSetArg(args[n], XtNdefaultColumns, 1);                  n++;
    XtSetArg(args[n], XtNforceColumns, True);                 n++;
    XtSetArg(args[n], XtNverticalList, True);                 n++;
    XtSetArg(args[n], XtNborderWidth, 0);                     n++;
    XtSetArg(args[n], XtNwidth, CAT_PANE_WIDTH);              n++;
    XtSetArg(args[n], XtNheight, MENU_HEIGHT - TOOLBAR_HEIGHT); n++;
    XtSetArg(args[n], XtNcursor, None);                       n++;
    XtSetArg(args[n], XtNbackground, cat_bg);                 n++;
    p->cat_box = XtCreateManagedWidget("catList", listWidgetClass,
                                       form, args, n);
    XtAddCallback(p->cat_box, XtNcallback, category_selected, p);
    /* Don't free cat_names — the List widget holds a pointer to it */

    /* Viewport for app list (right pane) — lighter tone, vertical scroll */
    n = 0;
    XtSetArg(args[n], XtNfromHoriz, p->cat_box);                  n++;
    XtSetArg(args[n], XtNwidth, MENU_WIDTH - CAT_PANE_WIDTH);    n++;
    XtSetArg(args[n], XtNheight, MENU_HEIGHT - TOOLBAR_HEIGHT);  n++;
    XtSetArg(args[n], XtNborderWidth, 0);                         n++;
    XtSetArg(args[n], XtNallowVert, True);                        n++;
    XtSetArg(args[n], XtNallowHoriz, False);                      n++;
    XtSetArg(args[n], XtNuseRight, True);                          n++;
    XtSetArg(args[n], XtNbackground, app_bg);                      n++;
    p->app_viewport = XtCreateManagedWidget("appViewport",
                                            viewportWidgetClass,
                                            form, args, n);

    /* App list — child of viewport.
     * Must be static since the List widget holds the pointer. */
    static String initial[] = { "Select a category", NULL };
    n = 0;
    XtSetArg(args[n], XtNlist, initial);                          n++;
    XtSetArg(args[n], XtNnumberStrings, 1);                       n++;
    XtSetArg(args[n], XtNdefaultColumns, 1);                      n++;
    XtSetArg(args[n], XtNforceColumns, True);                     n++;
    XtSetArg(args[n], XtNverticalList, True);                     n++;
    XtSetArg(args[n], XtNborderWidth, 0);                         n++;
    XtSetArg(args[n], XtNcursor, None);                            n++;
    XtSetArg(args[n], XtNbackground, app_bg);                      n++;
    p->app_box = XtCreateManagedWidget("appList", listWidgetClass,
                                       p->app_viewport, args, n);
    XtAddCallback(p->app_box, XtNcallback, app_selected, p);

    /* Category list: hover highlights and switches category immediately */
    static char catTranslations[] =
        "<EnterWindow>: Set()\n"
        "<LeaveWindow>: Unset()\n"
        "<Motion>:      Set() Notify()\n"
        "<BtnDown>:     Set() Notify()\n"
        "<BtnUp>:       Notify()";
    XtOverrideTranslations(p->cat_box,
                           XtParseTranslationTable(catTranslations));

    /* App list: hover highlights, click launches */
    static char appTranslations[] =
        "<EnterWindow>: Set()\n"
        "<LeaveWindow>: Unset()\n"
        "<Motion>:      Set()\n"
        "<BtnDown>:     Set() Notify()\n"
        "<BtnUp>:       Notify()";
    XtOverrideTranslations(p->app_box,
                           XtParseTranslationTable(appTranslations));

    /* Keyboard navigation via event handler on the shell */
    XtAddEventHandler(p->start_shell, XCB_EVENT_MASK_KEY_PRESS, False,
                      menu_key_handler, p);

    /* Bottom toolbar — right-aligned action buttons.
     * No defaultDistance override: the Form's default 4px acts as bottom margin,
     * so the natural height = vertDistance + btn_size + 4 = TOOLBAR_HEIGHT. */
    n = 0;
    XtSetArg(args[n], XtNfromVert, p->cat_box);             n++;
    XtSetArg(args[n], XtNvertDistance, 0);                   n++;
    XtSetArg(args[n], XtNwidth, MENU_WIDTH);                 n++;
    XtSetArg(args[n], XtNheight, TOOLBAR_HEIGHT);            n++;
    XtSetArg(args[n], XtNborderWidth, 0);                    n++;
    XtSetArg(args[n], XtNbackground, cat_bg);                n++;
    p->menu_toolbar = XtCreateManagedWidget("menuToolbar", formWidgetClass,
                                            form, args, n);

    /* btn_size = TOOLBAR_HEIGHT - top_margin(4) - bottom_margin(4) */
    int btn_margin = 4;
    int btn_size   = TOOLBAR_HEIGHT - btn_margin * 2;
    int btn_x      = MENU_WIDTH - btn_size - btn_margin;

    /* Logout button (rightmost) */
    n = 0;
    XtSetArg(args[n], XtNlabel, "");                         n++;
    XtSetArg(args[n], XtNwidth, btn_size);                   n++;
    XtSetArg(args[n], XtNheight, btn_size);                  n++;
    XtSetArg(args[n], XtNhorizDistance, btn_x);              n++;
    XtSetArg(args[n], XtNvertDistance, btn_margin);          n++;
    XtSetArg(args[n], XtNborderWidth, 1);                    n++;
    XtSetArg(args[n], XtNinternalWidth, 0);                  n++;
    XtSetArg(args[n], XtNinternalHeight, 0);                 n++;
    XtSetArg(args[n], XtNleft, XtChainRight);                n++;
    XtSetArg(args[n], XtNright, XtChainRight);               n++;
    if (logout_icon_path) {
        XtSetArg(args[n], XtNimage, logout_icon_path);       n++;
    }
    p->logout_btn = XtCreateManagedWidget("logoutBtn", commandWidgetClass,
                                          p->menu_toolbar, args, n);
    XtAddCallback(p->logout_btn, XtNcallback, logout_cb, p);

    /* Reboot button (left of logout) */
    btn_x -= btn_size + btn_margin;
    n = 0;
    XtSetArg(args[n], XtNlabel, "");                         n++;
    XtSetArg(args[n], XtNwidth, btn_size);                   n++;
    XtSetArg(args[n], XtNheight, btn_size);                  n++;
    XtSetArg(args[n], XtNhorizDistance, btn_x);              n++;
    XtSetArg(args[n], XtNvertDistance, btn_margin);          n++;
    XtSetArg(args[n], XtNborderWidth, 1);                    n++;
    XtSetArg(args[n], XtNinternalWidth, 0);                  n++;
    XtSetArg(args[n], XtNinternalHeight, 0);                 n++;
    XtSetArg(args[n], XtNleft, XtChainRight);                n++;
    XtSetArg(args[n], XtNright, XtChainRight);               n++;
    if (reboot_icon_path) {
        XtSetArg(args[n], XtNimage, reboot_icon_path);       n++;
    }
    p->reboot_btn = XtCreateManagedWidget("rebootBtn", commandWidgetClass,
                                          p->menu_toolbar, args, n);
    XtAddCallback(p->reboot_btn, XtNcallback, reboot_cb, p);

    /* Shut Down button (left of reboot) */
    btn_x -= btn_size + btn_margin;
    n = 0;
    XtSetArg(args[n], XtNlabel, "");                         n++;
    XtSetArg(args[n], XtNwidth, btn_size);                   n++;
    XtSetArg(args[n], XtNheight, btn_size);                  n++;
    XtSetArg(args[n], XtNhorizDistance, btn_x);              n++;
    XtSetArg(args[n], XtNvertDistance, btn_margin);          n++;
    XtSetArg(args[n], XtNborderWidth, 1);                    n++;
    XtSetArg(args[n], XtNinternalWidth, 0);                  n++;
    XtSetArg(args[n], XtNinternalHeight, 0);                 n++;
    XtSetArg(args[n], XtNleft, XtChainRight);                n++;
    XtSetArg(args[n], XtNright, XtChainRight);               n++;
    if (shutdown_icon_path) {
        XtSetArg(args[n], XtNimage, shutdown_icon_path);     n++;
    }
    p->shutdown_btn = XtCreateManagedWidget("shutdownBtn", commandWidgetClass,
                                            p->menu_toolbar, args, n);
    XtAddCallback(p->shutdown_btn, XtNcallback, shutdown_cb, p);

    /* Hide app list until a category is hovered */
    XtUnmapWidget(p->app_viewport);
}

void startmenu_cleanup(Panel *p)
{
    for (int i = 0; i < p->ncategories; i++) {
        free(p->categories[i].apps);
    }
    free(p->categories);
    p->categories = NULL;
    p->ncategories = 0;

    free(shutdown_icon_path);
    shutdown_icon_path = NULL;
    free(reboot_icon_path);
    reboot_icon_path = NULL;
    free(logout_icon_path);
    logout_icon_path = NULL;

    if (key_syms) {
        xcb_key_symbols_free(key_syms);
        key_syms = NULL;
    }
}
