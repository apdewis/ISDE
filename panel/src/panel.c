#define _POSIX_C_SOURCE 200809L
/*
 * panel.c — panel initialization, dock window, event integration
 */
#include "panel.h"
#include "panel-x11.h"
#include <ISW/ShellP.h>
#include <ISW/IswArgMacros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void load_pinned(Panel *p)
{
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (!cfg) {
        return;
    }

    IsdeConfigTable *root = isde_config_root(cfg);
    IsdeConfigTable *panel_cfg = isde_config_table(root, "panel");
    if (panel_cfg) {
        char **classes = NULL;
        int n = isde_config_string_array(panel_cfg, "pinned", &classes);
        if (n > 0 && classes) {
            p->pinned_classes = classes;
            p->npinned = n;
        }
    }
    isde_config_free(cfg);
}

static void load_desktop_entries(Panel *p)
{
    /* Scan XDG_DATA_DIRS/applications/ for .desktop files */
    const char *data_dirs = isde_xdg_data_dirs();
    const char *dp = data_dirs;

    while (dp && *dp) {
        const char *colon = strchr(dp, ':');
        size_t dlen = colon ? (size_t)(colon - dp) : strlen(dp);
        if (dlen > 0) {
            char path[512];
            snprintf(path, sizeof(path), "%.*s/applications",
                     (int)dlen, dp);
            int count = 0;
            IsdeDesktopEntry **entries =
                isde_desktop_scan_dir(path, &count);
            if (entries && count > 0) {
                p->desktop_entries = realloc(p->desktop_entries,
                    (p->ndesktop + count) * sizeof(IsdeDesktopEntry *));
                for (int i = 0; i < count; i++) {
                    p->desktop_entries[p->ndesktop++] = entries[i];
                }
                free(entries);
            }
        }
        dp = colon ? colon + 1 : NULL;
    }

    /* Also scan XDG_DATA_HOME/applications/ */
    char path[512];
    snprintf(path, sizeof(path), "%s/applications",
             isde_xdg_data_home());
    int count = 0;
    IsdeDesktopEntry **entries = isde_desktop_scan_dir(path, &count);
    if (entries && count > 0) {
        p->desktop_entries = realloc(p->desktop_entries,
            (p->ndesktop + count) * sizeof(IsdeDesktopEntry *));
        for (int i = 0; i < count; i++) {
            p->desktop_entries[p->ndesktop++] = entries[i];
        }
        free(entries);
    }
}

void panel_reload_desktop_entries(Panel *p)
{
    for (int i = 0; i < p->ndesktop; i++) {
        isde_desktop_free(p->desktop_entries[i]);
    }
    free(p->desktop_entries);
    p->desktop_entries = NULL;
    p->ndesktop = 0;

    load_desktop_entries(p);

    /* Taskbar groups cache indices into desktop_entries; invalidate them
     * so they aren't dereferenced against the new (renumbered) array.
     * Pre-built context menus retain any actions from the previous scan. */
    for (TaskGroup *g = p->groups; g; g = g->next) {
        g->desktop_index = -1;
    }
}

/* Find a .desktop entry whose WM_CLASS (StartupWMClass) or executable
 * name matches the given class */
static IsdeDesktopEntry *find_desktop_for_class(Panel *p,
                                                 const char *wm_class)
{
    if (!wm_class) {
        return NULL;
    }
    /* TODO: match StartupWMClass field when we add it to the parser.
     * For now, try matching the lowercase exec basename against
     * the lowercase class name. */
    char class_lower[128];
    int i;
    for (i = 0; wm_class[i] && i < 126; i++) {
        class_lower[i] = (wm_class[i] >= 'A' && wm_class[i] <= 'Z')
                        ? wm_class[i] + 32 : wm_class[i];
    }
    class_lower[i] = '\0';

    for (int j = 0; j < p->ndesktop; j++) {
        const char *exec = isde_desktop_exec(p->desktop_entries[j]);
        if (!exec) {
            continue;
        }
        /* Get basename of exec */
        const char *base = strrchr(exec, '/');
        base = base ? base + 1 : exec;
        /* Strip any arguments */
        char name[128];
        int k;
        for (k = 0; base[k] && base[k] != ' ' && k < 126; k++) {
            name[k] = (base[k] >= 'A' && base[k] <= 'Z')
                     ? base[k] + 32 : base[k];
        }
        name[k] = '\0';

        if (strcmp(name, class_lower) == 0) {
            return p->desktop_entries[j];
        }
    }
    return NULL;
}

static void panel_reconfigure(Panel *p);

/* Forward declarations */
static void on_panel_settings_changed(const char *, const char *, void *);
static void panel_dbus_input_cb(IswPointer, int *, IswInputId *);
static void on_panel_theme_changed(void *);

void panel_ipc_event_handler(Widget w, IswPointer client_data,
                             xcb_generic_event_t *xev, Boolean *cont)
{
    (void)w; (void)cont;
    Panel *p = (Panel *)client_data;

    uint32_t cmd;
    if (!isde_ipc_decode(p->ipc, xev, &cmd, NULL, NULL, NULL, NULL)) {
        return;
    }
    if (cmd == ISDE_CMD_TOGGLE_START_MENU) {
        startmenu_toggle(p);
    }
}

int panel_init(Panel *p, int *argc, char **argv)
{
    memset(p, 0, sizeof(*p));

    p->toplevel = IswAppInitialize(&p->app, "ISDE-Panel",
                                  NULL, 0, argc, argv,
                                  NULL, NULL, 0);
    isde_theme_merge_xrm(p->toplevel);

    if (panel_init_display(p) != 0) {
        return -1;
    }

    p->ewmh = isde_ewmh_init(p->conn, p->screen_num);
    p->ipc  = isde_ipc_init(p->conn, p->screen_num);

    /* Load config and desktop entries */
    load_pinned(p);
    load_desktop_entries(p);

    /* Query primary monitor geometry */
    query_primary_monitor(p);

    /* Convert monitor geometry to logical pixels — ISW scales shell
       dimensions during creation, so all sizes must be logical. */
    double sf = ISWScaleFactor(p->toplevel);
    int logical_mon_x = (int)(p->mon_x / sf + 0.5);
    int logical_mon_w = (int)(p->mon_w / sf + 0.5);
    int logical_mon_h = (int)(p->mon_h / sf + 0.5);
    int logical_mon_y = (int)(p->mon_y / sf + 0.5);

    /* Physical panel height for EWMH strut and IswConfigureWidget
       (both operate in physical pixels). */
    p->phys_panel_h = (int)(PANEL_HEIGHT * sf + 0.5);

    /* Create panel shell as OverrideShell for simple Xt geometry (no WM
       negotiation), then clear override_redirect on the X window so the
       WM receives MapRequest and can handle dock stacking. */
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgX(&ab, logical_mon_x);
    IswArgY(&ab, logical_mon_y + logical_mon_h - PANEL_HEIGHT);
    IswArgWidth(&ab, logical_mon_w);
    IswArgHeight(&ab, PANEL_HEIGHT);
    IswArgOverrideRedirect(&ab, True);
    IswArgBorderWidth(&ab, 0);
    p->shell = IswCreatePopupShell("panel", overrideShellWidgetClass,
                                  p->toplevel, ab.args, ab.count);

    /* FlexBox layout: start button | taskbar box | tray | clock */
    IswArgBuilderReset(&ab);
    IswArgOrientation(&ab, IswOrientHorizontal);
    IswArgSpacing(&ab, 0);
    IswArgBorderWidth(&ab, 0);
    p->form = IswCreateManagedWidget("panelFlex", flexBoxWidgetClass,
                                    p->shell, ab.args, ab.count);

    /* Initialize applets — they create widgets inside p->form */
    startmenu_init(p);
    pager_init(p);

    /* Taskbar box — flexGrow=1 fills remaining space */
    IswArgBuilderReset(&ab);
    IswArgOrientation(&ab, IswOrientHorizontal);
    IswArgBorderWidth(&ab, 0);
    IswArgHSpace(&ab, 2);
    IswArgVSpace(&ab, 0);
    IswArgHeight(&ab, PANEL_HEIGHT);
    IswArgFlexGrow(&ab, 1);
    p->box = IswCreateManagedWidget("panelBox", boxWidgetClass,
                                   p->form, ab.args, ab.count);

    taskbar_init(p);
    //tray_init_widgets(p);
    clock_init(p);

    IswRealizeWidget(p->shell);

    /* Clear override-redirect, set dock type + strut, select root events,
     * register the IPC handler, subscribe to RandR — all before mapping. */
    panel_setup_dock_window(p);

    IswMapWidget(p->shell);

    /* Claim system tray selection (needs realized window) */
    //tray_init_selection(p);

    /* D-Bus settings notifications */
    p->dbus = isde_dbus_init();
    if (p->dbus) {
        isde_theme_watch(p->dbus, p->toplevel, on_panel_theme_changed, p);
        isde_dbus_settings_subscribe(p->dbus, on_panel_settings_changed, p);
        int dbus_fd = isde_dbus_get_fd(p->dbus);
        if (dbus_fd >= 0) {
            IswAppAddInput(p->app, dbus_fd,
                          (IswPointer)IswInputReadMask,
                          panel_dbus_input_cb, p->dbus);
        }
    }

    p->running = 1;
    return 0;
}

/* ---------- reconfigure on monitor change ---------- */

static void panel_reconfigure(Panel *p)
{
    int16_t  old_x = p->mon_x;
    uint16_t old_w = p->mon_w, old_h = p->mon_h;

    query_primary_monitor(p);

    if (p->mon_x == old_x && p->mon_w == old_w && p->mon_h == old_h) {
        return;
    }

    /* All IswConfigureWidget values must be logical — ISW scales internally */
    double sf = ISWScaleFactor(p->toplevel);
    int log_x = (int)(p->mon_x / sf + 0.5);
    int log_w = (int)(p->mon_w / sf + 0.5);
    int log_y = (int)((p->mon_y + p->mon_h) / sf + 0.5) - PANEL_HEIGHT;

    IswConfigureWidget(p->shell, log_x, log_y, log_w, PANEL_HEIGHT, 0);

    /* The width change relaid out the panel FlexBox, shifting the windowless
     * tray container; re-offset the reparented icons to track it. */
    //tray_reposition_all(p);

    /* Update strut */
    panel_update_strut(p);

    fprintf(stderr, "isde-panel: reconfigured for %dx%d+%d+%d\n",
            p->mon_w, p->mon_h, p->mon_x, p->mon_y);
}

/* ---------- D-Bus settings reload ---------- */

/* Theme colors are applied via Xresources (isde_theme_merge_xrm).
 * No manual IswSetValues needed — all widget names match resource specs. */


static void on_panel_theme_changed(void *user_data)
{
    Panel *p = (Panel *)user_data;

    IswReloadResources(p->shell);

    for (TaskGroup *g = p->groups; g; g = g->next) {
        if (g->menu)
            IswReloadResources(g->menu);
        if (g->ctx_menu)
            IswReloadResources(g->ctx_menu);
    }

    if (p->start_shell)
        IswReloadResources(p->start_shell);
    if (p->cal_shell)
        IswReloadResources(p->cal_shell);

    taskbar_highlight_active(p);
    startmenu_reload_theme(p);
    calendar_reload_theme(p);
    //tray_set_colors(p);
}

static void on_panel_settings_changed(const char *section, const char *key,
                                      void *user_data)
{
    (void)key;
    Panel *p = (Panel *)user_data;
    if (strcmp(section, "panel.clock") == 0 ||
        strcmp(section, "panel") == 0) {
        p->running = 0;
        p->restart = 1;
    } else if (strcmp(section, "wm.desktops") == 0) {
        pager_reload_config(p);
    }
}

static void panel_dbus_input_cb(IswPointer client_data, int *fd, IswInputId *id)
{
    (void)fd;
    (void)id;
    IsdeDBus *bus = (IsdeDBus *)client_data;
    isde_dbus_dispatch(bus);
}

/* Periodic poll for client list changes — called every 50ms.
 * This catches _NET_CLIENT_LIST updates that Xt's event dispatch
 * would otherwise drop (PropertyNotify on root, no widget). */
static xcb_window_t last_active = XCB_WINDOW_NONE;

static int prev_client_count;

static void poll_clients(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    Panel *p = (Panel *)client_data;

    /* Check for screen changes (RandR) */
    panel_reconfigure(p);

    //tray_check_icons(p);
    taskbar_update(p);
    taskbar_highlight_active(p);
    pager_update(p);

    if (p->launch_id) {
        xcb_window_t *wins = NULL;
        int n = isde_ewmh_get_client_list(p->ewmh, &wins);
        free(wins);
        if (n > prev_client_count) {
            panel_clear_launch(p);
        }
        prev_client_count = n;
    }

    /* Dismiss popups when focus moves to a managed window */
    if (p->active_popup) {
        xcb_window_t active = isde_ewmh_get_active_window(p->ewmh);
        if (active != XCB_WINDOW_NONE && active != last_active) {
            panel_dismiss_popup(p);
        }
        last_active = active;
    } else {
        last_active = isde_ewmh_get_active_window(p->ewmh);
    }

    IswAppAddTimeOut(p->app, 50, poll_clients, p);
}

void panel_show_popup(Panel *p, Widget popup)
{
    panel_dismiss_popup(p);
    p->active_popup = popup;
}

void panel_dismiss_popup(Panel *p)
{
    if (!p->active_popup) {
        return;
    }

    panel_ungrab_popup(p);

    /* Reset start button to inactive state */
    if (p->active_popup == p->start_shell) {
        const IsdeColorScheme *s = isde_theme_current();
        if (s) {
            Pixel fg = panel_color_pixel(p, s->taskbar_button.fg);
            Pixel bg = panel_color_pixel(p, s->taskbar_button.bg);
            IswArgBuilder ab = IswArgBuilderInit();
            IswArgForeground(&ab, fg);
            IswArgBackground(&ab, bg);
            IswSetValues(p->start_btn, ab.args, ab.count);
        }
    }

    ShellWidget sw = (ShellWidget)p->active_popup;
    if (sw->shell.popped_up) {
        IswPopdown(p->active_popup);
    }

    p->active_popup = NULL;
}

/* ---------- startup notification / busy cursor ---------- */

#define LAUNCH_TIMEOUT_MS 15000

static void launch_timer_cb(IswPointer cd, IswIntervalId *id)
{
    (void)id;
    Panel *p = (Panel *)cd;
    panel_clear_launch(p);
}

void panel_clear_launch(Panel *p)
{
    if (p->launch_timer) {
        IswRemoveTimeOut(p->launch_timer);
        p->launch_timer = 0;
    }
    free(p->launch_id);
    p->launch_id = NULL;
    if (p->cursor_default) {
        set_panel_cursor(p, p->cursor_default);
    }
}

void panel_launch_notify(Panel *p, IsdeDesktopEntry *de,
                         const char **files, int nfiles)
{
    launch_cursor_init(p);
    panel_clear_launch(p);

    char *id = NULL;
    isde_desktop_launch_notify(de, files, nfiles, p->ewmh, &id);
    if (id) {
        p->launch_id = id;
        if (p->cursor_watch) {
            set_panel_cursor(p, p->cursor_watch);
        }
        p->launch_timer = IswAppAddTimeOut(p->app, LAUNCH_TIMEOUT_MS,
                                           launch_timer_cb, p);
    }
}

void panel_launch_cmd_notify(Panel *p, const char *cmd)
{
    isde_desktop_launch_cmd(cmd);
}

void panel_run(Panel *p)
{
    /* Initial taskbar population */
    taskbar_update(p);

    /* Start periodic client list poll */
    IswAppAddTimeOut(p->app, 50, poll_clients, p);

    while (p->running && !IswAppGetExitFlag(p->app)) {
        IswAppProcessEvent(p->app, IswIMAll);
    }
}

void panel_cleanup(Panel *p)
{
    panel_clear_launch(p);
    clock_cleanup(p);
    //tray_cleanup(p);
    taskbar_cleanup(p);
    pager_cleanup(p);
    startmenu_cleanup(p);

    for (int i = 0; i < p->ndesktop; i++) {
        isde_desktop_free(p->desktop_entries[i]);
    }
    free(p->desktop_entries);

    for (int i = 0; i < p->npinned; i++) {
        free(p->pinned_classes[i]);
    }
    free(p->pinned_classes);

    isde_dbus_free(p->dbus);
    isde_ipc_free(p->ipc);
    isde_ewmh_free(p->ewmh);
    IswDestroyApplicationContext(p->app);
}
