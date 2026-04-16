#define _POSIX_C_SOURCE 200809L
/*
 * startmenu.c — custom two-pane start menu
 *
 * OverrideShell with two List widgets:
 *   Left:  category names — click to populate right pane
 *   Right: app names for selected category — click to launch
 */
#include "panel.h"
#include <ISW/ShellP.h>
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
    xcb_connection_t *conn = IswDisplay(p->start_btn);
    xcb_screen_t *screen = IswScreen(p->start_btn);
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
        IswSetArg(args[0], IswNforeground,
                 start_color_pixel(p, s->taskbar_button.hover_fg));
        IswSetArg(args[1], IswNbackground,
                 start_color_pixel(p, s->active));
    } else {
        IswSetArg(args[0], IswNforeground,
                 start_color_pixel(p, s->taskbar_button.fg));
        IswSetArg(args[1], IswNbackground,
                 start_color_pixel(p, s->taskbar_button.bg));
    }
    IswSetValues(p->start_btn, args, 2);
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
    IswMapWidget(p->app_viewport);
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

static void category_selected(Widget w, IswPointer client_data,
                              IswPointer call_data)
{
    (void)w;
    Panel *p = (Panel *)client_data;
    IswListReturnStruct *ret = (IswListReturnStruct *)call_data;
    if (ret->list_index == XAW_LIST_NONE) {
        return;
    }
    show_category(p, ret->list_index);
}

static void app_selected(Widget w, IswPointer client_data,
                         IswPointer call_data)
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

static void menu_key_handler(Widget w, IswPointer client_data,
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

static void shutdown_cb(Widget w, IswPointer client_data, IswPointer call_data)
{
    (void)w; (void)call_data;
    Panel *p = (Panel *)client_data;
    panel_dismiss_popup(p);
    isde_ipc_send(p->ipc, ISDE_CMD_SHUTDOWN, 0, 0, 0, 0);
}

static void reboot_cb(Widget w, IswPointer client_data, IswPointer call_data)
{
    (void)w; (void)call_data;
    Panel *p = (Panel *)client_data;
    panel_dismiss_popup(p);
    isde_ipc_send(p->ipc, ISDE_CMD_REBOOT, 0, 0, 0, 0);
}

static void logout_cb(Widget w, IswPointer client_data, IswPointer call_data)
{
    (void)w; (void)call_data;
    Panel *p = (Panel *)client_data;
    panel_dismiss_popup(p);
    isde_ipc_send(p->ipc, ISDE_CMD_LOGOUT, 0, 0, 0, 0);
}

/* ---------- toggle ---------- */

static void toggle_start_menu(Widget w, IswPointer client_data,
                              IswPointer call_data)
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
     * All values must be logical — ISW scales to physical internally. */
    double sf = ISWScaleFactor(p->toplevel);
    int log_mon_x = (int)(p->mon_x / sf + 0.5);
    int log_panel_top = (int)((p->mon_y + p->mon_h) / sf + 0.5)
                        - PANEL_HEIGHT;

    if (!IswIsRealized(p->start_shell)) {
        IswRealizeWidget(p->start_shell);
    }
    int menu_w = p->start_shell->core.width;
    int menu_h = p->start_shell->core.height;
    int menu_y = log_panel_top - menu_h;
    IswConfigureWidget(p->start_shell, log_mon_x, menu_y,
                      menu_w, menu_h, 1);
    IswPopup(p->start_shell, IswGrabNone);

    /* Force immediate redraw — the shell content may be stale from the
     * previous popdown, causing a blank menu on some redraws. */
    IswExposeProc expose = IswClass(p->start_shell)->core_class.expose;
    if (expose) {
        expose(p->start_shell, NULL, 0);
    }

    panel_show_popup(p, p->start_shell);
    p->active_cat = -1;
    p->cat_highlight = 0;
    p->app_highlight = -1;
    p->menu_focus = 0;
    IswUnmapWidget(p->app_viewport);

    /* Highlight first category and grab keyboard */
    IswListHighlight(p->cat_box, 0);
    show_category(p, 0);
    xcb_grab_keyboard(p->conn, 1, IswWindow(p->start_shell),
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
        IswSetArg(args[n], IswNimage, start_icon_path);       n++;
    }
    IswSetArg(args[n], IswNlabel, "");                 n++;
    IswSetArg(args[n], IswNwidth, PANEL_HEIGHT);       n++;
    IswSetArg(args[n], IswNheight, PANEL_HEIGHT);      n++;
    IswSetArg(args[n], IswNinternalWidth, 0);          n++;
    IswSetArg(args[n], IswNinternalHeight, 0);         n++;
    IswSetArg(args[n], IswNflexBasis, PANEL_HEIGHT);   n++;
    IswSetArg(args[n], IswNborderWidth, 0);            n++;
    p->start_btn = IswCreateManagedWidget("startBtn", commandWidgetClass,
                                         p->form, args, n);
    IswAddCallback(p->start_btn, IswNcallback, toggle_start_menu, p);

    /* Start menu shell */
    n = 0;
    const IsdeColorScheme *scheme_border = isde_theme_current();
    Pixel border_px = scheme_border
        ? start_color_pixel(p, scheme_border->border)
        : IswScreen(p->start_btn)->white_pixel;
    IswSetArg(args[n], IswNwidth, MENU_WIDTH);           n++;
    IswSetArg(args[n], IswNheight, MENU_HEIGHT);         n++;
    IswSetArg(args[n], IswNoverrideRedirect, True);      n++;
    IswSetArg(args[n], IswNborderWidth, 1);              n++;
    IswSetArg(args[n], IswNborderColor, border_px);      n++;
    p->start_shell = IswCreatePopupShell("startMenu",
                                        overrideShellWidgetClass,
                                        p->start_btn, args, n);

    /* Form container — single child of the shell */
    n = 0;
    IswSetArg(args[n], IswNdefaultDistance, 0);  n++;
    IswSetArg(args[n], IswNborderWidth, 0);      n++;
    Widget form = IswCreateManagedWidget("menuForm", formWidgetClass,
                                        p->start_shell, args, n);

    /* Pane background tones from theme */
    const IsdeColorScheme *scheme = isde_theme_current();
    Pixel cat_bg  = scheme ? start_color_pixel(p, scheme->bg)
                           : IswScreen(p->start_btn)->white_pixel;
    Pixel app_bg  = scheme ? start_color_pixel(p, scheme->bg_light)
                           : IswScreen(p->start_btn)->white_pixel;

    /* Viewport for category list (left pane) — darker tone, vertical scroll */
    n = 0;
    IswSetArg(args[n], IswNwidth, CAT_PANE_WIDTH);              n++;
    IswSetArg(args[n], IswNheight, MENU_HEIGHT - TOOLBAR_HEIGHT); n++;
    IswSetArg(args[n], IswNborderWidth, 0);                     n++;
    IswSetArg(args[n], IswNallowVert, True);                    n++;
    IswSetArg(args[n], IswNallowHoriz, False);                  n++;
    IswSetArg(args[n], IswNbackground, cat_bg);                 n++;
    p->cat_viewport = IswCreateManagedWidget("catViewport",
                                            viewportWidgetClass,
                                            form, args, n);

    /* Category list — child of viewport */
    String *cat_names = malloc((p->ncategories + 1) * sizeof(String));
    for (int i = 0; i < p->ncategories; i++) {
        cat_names[i] = (String)p->categories[i].label;
    }
    cat_names[p->ncategories] = NULL;

    n = 0;
    IswSetArg(args[n], IswNlist, cat_names);                    n++;
    IswSetArg(args[n], IswNnumberStrings, p->ncategories);     n++;
    IswSetArg(args[n], IswNdefaultColumns, 1);                  n++;
    IswSetArg(args[n], IswNforceColumns, True);                 n++;
    IswSetArg(args[n], IswNverticalList, True);                 n++;
    IswSetArg(args[n], IswNborderWidth, 0);                     n++;
    IswSetArg(args[n], IswNwidth, CAT_PANE_WIDTH);              n++;
    IswSetArg(args[n], IswNcursor, None);                       n++;
    IswSetArg(args[n], IswNbackground, cat_bg);                 n++;
    p->cat_box = IswCreateManagedWidget("catList", listWidgetClass,
                                       p->cat_viewport, args, n);
    IswAddCallback(p->cat_box, IswNcallback, category_selected, p);
    /* Don't free cat_names — the List widget holds a pointer to it */

    /* Viewport for app list (right pane) — lighter tone, vertical scroll */
    n = 0;
    IswSetArg(args[n], IswNfromHoriz, p->cat_viewport);              n++;
    IswSetArg(args[n], IswNwidth, MENU_WIDTH - CAT_PANE_WIDTH);    n++;
    IswSetArg(args[n], IswNheight, MENU_HEIGHT - TOOLBAR_HEIGHT);  n++;
    IswSetArg(args[n], IswNborderWidth, 0);                         n++;
    IswSetArg(args[n], IswNallowVert, True);                        n++;
    IswSetArg(args[n], IswNallowHoriz, False);                      n++;
    IswSetArg(args[n], IswNuseRight, True);                          n++;
    IswSetArg(args[n], IswNbackground, app_bg);                      n++;
    p->app_viewport = IswCreateManagedWidget("appViewport",
                                            viewportWidgetClass,
                                            form, args, n);

    /* App list — child of viewport.
     * Must be static since the List widget holds the pointer. */
    static String initial[] = { "Select a category", NULL };
    n = 0;
    IswSetArg(args[n], IswNlist, initial);                          n++;
    IswSetArg(args[n], IswNnumberStrings, 1);                       n++;
    IswSetArg(args[n], IswNdefaultColumns, 1);                      n++;
    IswSetArg(args[n], IswNforceColumns, True);                     n++;
    IswSetArg(args[n], IswNverticalList, True);                     n++;
    IswSetArg(args[n], IswNborderWidth, 0);                         n++;
    IswSetArg(args[n], IswNheight, MENU_HEIGHT - TOOLBAR_HEIGHT);  n++;
    IswSetArg(args[n], IswNcursor, None);                            n++;
    IswSetArg(args[n], IswNbackground, app_bg);                      n++;
    p->app_box = IswCreateManagedWidget("appList", listWidgetClass,
                                       p->app_viewport, args, n);
    IswAddCallback(p->app_box, IswNcallback, app_selected, p);

    /* Category list: hover highlights and switches category immediately */
    static char catTranslations[] =
        "<EnterWindow>: Set()\n"
        "<LeaveWindow>: Unset()\n"
        "<Motion>:      Set() Notify()\n"
        "<BtnDown>:     Set() Notify()\n"
        "<BtnUp>:       Notify()";
    IswOverrideTranslations(p->cat_box,
                           IswParseTranslationTable(catTranslations));

    /* App list: hover highlights, click launches */
    static char appTranslations[] =
        "<EnterWindow>: Set()\n"
        "<LeaveWindow>: Unset()\n"
        "<Motion>:      Set()\n"
        "<BtnDown>:     Set() Notify()\n"
        "<BtnUp>:       Notify()";
    IswOverrideTranslations(p->app_box,
                           IswParseTranslationTable(appTranslations));

    /* Keyboard navigation via event handler on the shell */
    IswAddEventHandler(p->start_shell, XCB_EVENT_MASK_KEY_PRESS, False,
                      menu_key_handler, p);

    /* Bottom toolbar — right-aligned action buttons.
     * No defaultDistance override: the Form's default 4px acts as bottom margin,
     * so the natural height = vertDistance + btn_size + 4 = TOOLBAR_HEIGHT. */
    n = 0;
    IswSetArg(args[n], IswNfromVert, p->cat_viewport);         n++;
    IswSetArg(args[n], IswNvertDistance, 0);                   n++;
    IswSetArg(args[n], IswNwidth, MENU_WIDTH);                 n++;
    IswSetArg(args[n], IswNheight, TOOLBAR_HEIGHT);            n++;
    IswSetArg(args[n], IswNborderWidth, 0);                    n++;
    IswSetArg(args[n], IswNbackground, cat_bg);                n++;
    p->menu_toolbar = IswCreateManagedWidget("menuToolbar", formWidgetClass,
                                            form, args, n);

    /* btn_size = TOOLBAR_HEIGHT - top_margin(4) - bottom_margin(4) */
    int btn_margin = 4;
    int btn_size   = TOOLBAR_HEIGHT - btn_margin * 2;
    int btn_x      = MENU_WIDTH - btn_size - btn_margin;

    /* Logout button (rightmost) */
    n = 0;
    IswSetArg(args[n], IswNlabel, "");                         n++;
    IswSetArg(args[n], IswNwidth, btn_size);                   n++;
    IswSetArg(args[n], IswNheight, btn_size);                  n++;
    IswSetArg(args[n], IswNhorizDistance, btn_x);              n++;
    IswSetArg(args[n], IswNvertDistance, btn_margin);          n++;
    IswSetArg(args[n], IswNborderWidth, 1);                    n++;
    IswSetArg(args[n], IswNinternalWidth, 0);                  n++;
    IswSetArg(args[n], IswNinternalHeight, 0);                 n++;
    IswSetArg(args[n], IswNleft, IswChainRight);                n++;
    IswSetArg(args[n], IswNright, IswChainRight);               n++;
    if (logout_icon_path) {
        IswSetArg(args[n], IswNimage, logout_icon_path);       n++;
    }
    p->logout_btn = IswCreateManagedWidget("logoutBtn", commandWidgetClass,
                                          p->menu_toolbar, args, n);
    IswAddCallback(p->logout_btn, IswNcallback, logout_cb, p);

    /* Reboot button (left of logout) */
    btn_x -= btn_size + btn_margin;
    n = 0;
    IswSetArg(args[n], IswNlabel, "");                         n++;
    IswSetArg(args[n], IswNwidth, btn_size);                   n++;
    IswSetArg(args[n], IswNheight, btn_size);                  n++;
    IswSetArg(args[n], IswNhorizDistance, btn_x);              n++;
    IswSetArg(args[n], IswNvertDistance, btn_margin);          n++;
    IswSetArg(args[n], IswNborderWidth, 1);                    n++;
    IswSetArg(args[n], IswNinternalWidth, 0);                  n++;
    IswSetArg(args[n], IswNinternalHeight, 0);                 n++;
    IswSetArg(args[n], IswNleft, IswChainRight);                n++;
    IswSetArg(args[n], IswNright, IswChainRight);               n++;
    if (reboot_icon_path) {
        IswSetArg(args[n], IswNimage, reboot_icon_path);       n++;
    }
    p->reboot_btn = IswCreateManagedWidget("rebootBtn", commandWidgetClass,
                                          p->menu_toolbar, args, n);
    IswAddCallback(p->reboot_btn, IswNcallback, reboot_cb, p);

    /* Shut Down button (left of reboot) */
    btn_x -= btn_size + btn_margin;
    n = 0;
    IswSetArg(args[n], IswNlabel, "");                         n++;
    IswSetArg(args[n], IswNwidth, btn_size);                   n++;
    IswSetArg(args[n], IswNheight, btn_size);                  n++;
    IswSetArg(args[n], IswNhorizDistance, btn_x);              n++;
    IswSetArg(args[n], IswNvertDistance, btn_margin);          n++;
    IswSetArg(args[n], IswNborderWidth, 1);                    n++;
    IswSetArg(args[n], IswNinternalWidth, 0);                  n++;
    IswSetArg(args[n], IswNinternalHeight, 0);                 n++;
    IswSetArg(args[n], IswNleft, IswChainRight);                n++;
    IswSetArg(args[n], IswNright, IswChainRight);               n++;
    if (shutdown_icon_path) {
        IswSetArg(args[n], IswNimage, shutdown_icon_path);     n++;
    }
    p->shutdown_btn = IswCreateManagedWidget("shutdownBtn", commandWidgetClass,
                                            p->menu_toolbar, args, n);
    IswAddCallback(p->shutdown_btn, IswNcallback, shutdown_cb, p);

    /* Hide app list until a category is hovered */
    IswUnmapWidget(p->app_viewport);
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
