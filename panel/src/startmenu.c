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
#include <ISW/IswArgMacros.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/inotify.h>
#endif

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
    IswArgBuilder ab = IswArgBuilderInit();
    if (active) {
        IswArgForeground(&ab, start_color_pixel(p, s->taskbar_button.hover_fg));
        IswArgBackground(&ab, start_color_pixel(p, s->active));
    } else {
        IswArgForeground(&ab, start_color_pixel(p, s->taskbar_button.fg));
        IswArgBackground(&ab, start_color_pixel(p, s->taskbar_button.bg));
    }
    IswSetValues(p->start_btn, ab.args, ab.count);
}

#define MENU_WIDTH       600
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
    case XK_Super_L:
    case XK_Super_R:
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

static void toggle_start_menu_cb(Widget w, IswPointer client_data,
                                 IswPointer call_data)
{
    (void)w;
    (void)call_data;
    startmenu_toggle((Panel *)client_data);
}

void startmenu_toggle(Panel *p)
{
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

/* ---------- refresh / watch ---------- */

/* Backing array for cat_box strings; List widget keeps the pointer, so we
 * retain ownership here and free the previous array on each rebuild. */
static String *cat_names_backing;

static void free_categories(Panel *p)
{
    for (int i = 0; i < p->ncategories; i++) {
        free(p->categories[i].apps);
    }
    free(p->categories);
    p->categories = NULL;
    p->ncategories = 0;
}

static void startmenu_refresh(Panel *p)
{
    /* Menu is closed on refresh so widget pointers stay valid but nothing
     * is currently displayed from the stale data. */
    if (p->active_popup == p->start_shell) {
        panel_dismiss_popup(p);
    }

    free_categories(p);
    panel_reload_desktop_entries(p);
    build_categories(p);

    String *names = malloc((p->ncategories + 1) * sizeof(String));
    for (int i = 0; i < p->ncategories; i++) {
        names[i] = (String)p->categories[i].label;
    }
    names[p->ncategories] = NULL;

    IswListChange(p->cat_box, names, p->ncategories, 0, True);
    free(cat_names_backing);
    cat_names_backing = names;

    /* App pane shows stale pointers into freed categories; clear it. */
    static String empty[] = { NULL };
    IswListChange(p->app_box, empty, 0, 0, True);
    IswUnmapWidget(p->app_viewport);

    p->active_cat = -1;
    p->cat_highlight = -1;
    p->app_highlight = -1;
}

#ifdef __linux__
#define DESKTOP_WATCH_MASK (IN_CREATE | IN_DELETE | IN_MOVED_FROM | \
                            IN_MOVED_TO | IN_MODIFY | IN_ATTRIB)
#define DESKTOP_REFRESH_DEBOUNCE_MS 300

static void desktop_refresh_timer_cb(IswPointer cd, IswIntervalId *id)
{
    (void)id;
    Panel *p = (Panel *)cd;
    p->desktop_refresh_timer = 0;
    startmenu_refresh(p);
}

static void desktop_watch_cb(IswPointer cd, int *fd, IswInputId *id)
{
    (void)id;
    Panel *p = (Panel *)cd;

    char buf[4096]
        __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t len;
    int saw_desktop = 0;

    while ((len = read(*fd, buf, sizeof(buf))) > 0) {
        for (char *ptr = buf; ptr < buf + len; ) {
            struct inotify_event *ev = (struct inotify_event *)ptr;
            if (ev->len > 0) {
                /* Only care about .desktop files */
                const char *dot = strrchr(ev->name, '.');
                if (dot && strcmp(dot, ".desktop") == 0) {
                    saw_desktop = 1;
                }
            }
            ptr += sizeof(*ev) + ev->len;
        }
    }

    if (saw_desktop) {
        if (p->desktop_refresh_timer) {
            IswRemoveTimeOut(p->desktop_refresh_timer);
        }
        p->desktop_refresh_timer = IswAppAddTimeOut(
            p->app, DESKTOP_REFRESH_DEBOUNCE_MS,
            desktop_refresh_timer_cb, p);
    }
}

static void desktop_watch_add_dir(Panel *p, const char *path)
{
    /* Non-existent dirs are skipped; directories created later won't be
     * picked up without a restart, but that's the usual tradeoff. */
    inotify_add_watch(p->desktop_inotify_fd, path, DESKTOP_WATCH_MASK);
}

static void desktop_watch_start(Panel *p)
{
    p->desktop_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (p->desktop_inotify_fd < 0) {
        return;
    }

    const char *data_dirs = isde_xdg_data_dirs();
    const char *dp = data_dirs;
    while (dp && *dp) {
        const char *colon = strchr(dp, ':');
        size_t dlen = colon ? (size_t)(colon - dp) : strlen(dp);
        if (dlen > 0) {
            char path[512];
            snprintf(path, sizeof(path), "%.*s/applications",
                     (int)dlen, dp);
            desktop_watch_add_dir(p, path);
        }
        dp = colon ? colon + 1 : NULL;
    }

    char home_path[512];
    snprintf(home_path, sizeof(home_path), "%s/applications",
             isde_xdg_data_home());
    desktop_watch_add_dir(p, home_path);

    p->desktop_input_id = IswAppAddInput(p->app, p->desktop_inotify_fd,
                                         (IswPointer)IswInputReadMask,
                                         desktop_watch_cb, p);
}

static void desktop_watch_stop(Panel *p)
{
    if (p->desktop_refresh_timer) {
        IswRemoveTimeOut(p->desktop_refresh_timer);
        p->desktop_refresh_timer = 0;
    }
    if (p->desktop_input_id) {
        IswRemoveInput(p->desktop_input_id);
        p->desktop_input_id = 0;
    }
    if (p->desktop_inotify_fd >= 0) {
        close(p->desktop_inotify_fd);
        p->desktop_inotify_fd = -1;
    }
}
#else
static void desktop_watch_start(Panel *p) { (void)p; }
static void desktop_watch_stop(Panel *p) { (void)p; }
#endif

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
    IswArgBuilder ab = IswArgBuilderInit();
    if (start_icon_path) {
        IswArgImage(&ab, start_icon_path);
    }
    IswArgLabel(&ab, "");
    IswArgWidth(&ab, PANEL_HEIGHT);
    IswArgHeight(&ab, PANEL_HEIGHT);
    IswArgInternalWidth(&ab, 0);
    IswArgInternalHeight(&ab, 0);
    IswArgFlexBasis(&ab, PANEL_HEIGHT);
    IswArgBorderWidth(&ab, 0);
    p->start_btn = IswCreateManagedWidget("startBtn", commandWidgetClass,
                                         p->form, ab.args, ab.count);
    IswAddCallback(p->start_btn, IswNcallback, toggle_start_menu_cb, p);

    /* Start menu shell */
    const IsdeColorScheme *scheme_border = isde_theme_current();
    Pixel border_px = scheme_border
        ? start_color_pixel(p, scheme_border->border)
        : IswScreen(p->start_btn)->white_pixel;
    IswArgBuilderReset(&ab);
    IswArgWidth(&ab, MENU_WIDTH);
    IswArgHeight(&ab, MENU_HEIGHT);
    IswArgOverrideRedirect(&ab, True);
    IswArgBorderWidth(&ab, 1);
    IswArgBorderColor(&ab, border_px);
    p->start_shell = IswCreatePopupShell("startMenu",
                                        overrideShellWidgetClass,
                                        p->start_btn, ab.args, ab.count);

    /* Form container — single child of the shell */
    IswArgBuilderReset(&ab);
    IswArgDefaultDistance(&ab, 0);
    IswArgBorderWidth(&ab, 0);
    Widget form = IswCreateManagedWidget("menuForm", formWidgetClass,
                                        p->start_shell, ab.args, ab.count);

    /* Pane background tones from theme */
    const IsdeColorScheme *scheme = isde_theme_current();
    Pixel cat_bg  = scheme ? start_color_pixel(p, scheme->bg_light)
                           : IswScreen(p->start_btn)->white_pixel;
    Pixel app_bg  = scheme ? start_color_pixel(p, scheme->bg)
                           : IswScreen(p->start_btn)->white_pixel;

    /* Viewport for category list (left pane) — darker tone, vertical scroll */
    IswArgBuilderReset(&ab);
    IswArgWidth(&ab, CAT_PANE_WIDTH);
    IswArgHeight(&ab, MENU_HEIGHT - TOOLBAR_HEIGHT);
    IswArgBorderWidth(&ab, 1);
    IswArgAllowVert(&ab, True);
    IswArgAllowHoriz(&ab, False);
    IswArgBackground(&ab, cat_bg);
    p->cat_viewport = IswCreateManagedWidget("catViewport",
                                            viewportWidgetClass,
                                            form, ab.args, ab.count);

    /* Category list — child of viewport */
    String *cat_names = malloc((p->ncategories + 1) * sizeof(String));
    for (int i = 0; i < p->ncategories; i++) {
        cat_names[i] = (String)p->categories[i].label;
    }
    cat_names[p->ncategories] = NULL;

    IswArgBuilderReset(&ab);
    IswArgList(&ab, cat_names);
    IswArgNumberStrings(&ab, p->ncategories);
    IswArgDefaultColumns(&ab, 1);
    IswArgForceColumns(&ab, True);
    IswArgVerticalList(&ab, True);
    IswArgBorderWidth(&ab, 1);
    IswArgWidth(&ab, CAT_PANE_WIDTH);
    IswArgCursor(&ab, None);
    IswArgBackground(&ab, cat_bg);
    p->cat_box = IswCreateManagedWidget("catList", listWidgetClass,
                                       p->cat_viewport, ab.args, ab.count);
    IswAddCallback(p->cat_box, IswNcallback, category_selected, p);
    /* List widget holds the pointer — track it for later free on refresh */
    cat_names_backing = cat_names;

    /* Viewport for app list (right pane) — lighter tone, vertical scroll */
    IswArgBuilderReset(&ab);
    IswArgFromHoriz(&ab, p->cat_viewport);
    IswArgWidth(&ab, MENU_WIDTH - CAT_PANE_WIDTH);
    IswArgHeight(&ab, MENU_HEIGHT - TOOLBAR_HEIGHT);
    IswArgBorderWidth(&ab, 1);
    IswArgAllowVert(&ab, True);
    IswArgAllowHoriz(&ab, False);
    IswArgUseRight(&ab, True);
    IswArgBackground(&ab, app_bg);
    p->app_viewport = IswCreateManagedWidget("appViewport",
                                            viewportWidgetClass,
                                            form, ab.args, ab.count);

    /* App list — child of viewport.
     * Must be static since the List widget holds the pointer. */
    static String initial[] = { "Select a category", NULL };
    IswArgBuilderReset(&ab);
    IswArgList(&ab, initial);
    IswArgNumberStrings(&ab, 1);
    IswArgDefaultColumns(&ab, 1);
    IswArgForceColumns(&ab, True);
    IswArgVerticalList(&ab, True);
    IswArgBorderWidth(&ab, 1);
    IswArgHeight(&ab, MENU_HEIGHT - TOOLBAR_HEIGHT);
    IswArgCursor(&ab, None);
    IswArgBackground(&ab, app_bg);
    p->app_box = IswCreateManagedWidget("appList", listWidgetClass,
                                       p->app_viewport, ab.args, ab.count);
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
    IswArgBuilderReset(&ab);
    IswArgFromVert(&ab, p->cat_viewport);
    IswArgVertDistance(&ab, 0);
    IswArgWidth(&ab, MENU_WIDTH);
    IswArgHeight(&ab, TOOLBAR_HEIGHT);
    IswArgBorderWidth(&ab, 1);
    IswArgBackground(&ab, cat_bg);
    p->menu_toolbar = IswCreateManagedWidget("menuToolbar", formWidgetClass,
                                            form, ab.args, ab.count);

    /* btn_size = TOOLBAR_HEIGHT - top_margin(4) - bottom_margin(4) */
    int btn_margin = 4;
    int btn_size   = TOOLBAR_HEIGHT - btn_margin * 2;
    int btn_x      = MENU_WIDTH - btn_size - btn_margin;

    /* Logout button (rightmost) */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "");
    IswArgWidth(&ab, btn_size);
    IswArgHeight(&ab, btn_size);
    IswArgHorizDistance(&ab, btn_x);
    IswArgVertDistance(&ab, btn_margin);
    IswArgBorderWidth(&ab, 1);
    IswArgInternalWidth(&ab, 0);
    IswArgInternalHeight(&ab, 0);
    IswArgLeft(&ab, IswChainRight);
    IswArgRight(&ab, IswChainRight);
    if (logout_icon_path) {
        IswArgImage(&ab, logout_icon_path);
    }
    p->logout_btn = IswCreateManagedWidget("logoutBtn", commandWidgetClass,
                                          p->menu_toolbar, ab.args, ab.count);
    IswAddCallback(p->logout_btn, IswNcallback, logout_cb, p);

    /* Reboot button (left of logout) */
    btn_x -= btn_size + btn_margin;
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "");
    IswArgWidth(&ab, btn_size);
    IswArgHeight(&ab, btn_size);
    IswArgHorizDistance(&ab, btn_x);
    IswArgVertDistance(&ab, btn_margin);
    IswArgBorderWidth(&ab, 1);
    IswArgInternalWidth(&ab, 0);
    IswArgInternalHeight(&ab, 0);
    IswArgLeft(&ab, IswChainRight);
    IswArgRight(&ab, IswChainRight);
    if (reboot_icon_path) {
        IswArgImage(&ab, reboot_icon_path);
    }
    p->reboot_btn = IswCreateManagedWidget("rebootBtn", commandWidgetClass,
                                          p->menu_toolbar, ab.args, ab.count);
    IswAddCallback(p->reboot_btn, IswNcallback, reboot_cb, p);

    /* Shut Down button (left of reboot) */
    btn_x -= btn_size + btn_margin;
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "");
    IswArgWidth(&ab, btn_size);
    IswArgHeight(&ab, btn_size);
    IswArgHorizDistance(&ab, btn_x);
    IswArgVertDistance(&ab, btn_margin);
    IswArgBorderWidth(&ab, 1);
    IswArgInternalWidth(&ab, 0);
    IswArgInternalHeight(&ab, 0);
    IswArgLeft(&ab, IswChainRight);
    IswArgRight(&ab, IswChainRight);
    if (shutdown_icon_path) {
        IswArgImage(&ab, shutdown_icon_path);
    }
    p->shutdown_btn = IswCreateManagedWidget("shutdownBtn", commandWidgetClass,
                                            p->menu_toolbar, ab.args, ab.count);
    IswAddCallback(p->shutdown_btn, IswNcallback, shutdown_cb, p);

    /* Hide app list until a category is hovered */
    IswUnmapWidget(p->app_viewport);

    p->desktop_inotify_fd = -1;
    desktop_watch_start(p);
}

void startmenu_cleanup(Panel *p)
{
    desktop_watch_stop(p);

    free_categories(p);
    free(cat_names_backing);
    cat_names_backing = NULL;

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
