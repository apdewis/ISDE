#define _POSIX_C_SOURCE 200809L
/*
 * panel.c — panel initialization, dock window, event integration
 */
#include "panel.h"
#include "panel-x11.h"
#include "tray-net.h"
#include "tray-audio.h"
#include "tray-battery.h"
#include "tray-bt.h"
#include "tray-mount.h"
#include "../../platform/common/dbus.h"
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

/* Forward declarations */
static void on_panel_settings_changed(const char *, const char *, void *);
static void panel_dbus_input_cb(IswPointer, int *, IswInputId *);
static void on_panel_theme_changed(void *);

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

    panel_init_platform(p);
    panel_init_display_platform(p);
   


    /* Load config and desktop entries */
    load_pinned(p);
    load_desktop_entries(p);

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

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgX(&ab, logical_mon_x);
    IswArgY(&ab, logical_mon_y + logical_mon_h - PANEL_HEIGHT);
    IswArgWidth(&ab, logical_mon_w);
    IswArgHeight(&ab, PANEL_HEIGHT);
    IswArgBorderWidth(&ab, 0);
    ISW_ARG(&ab, IswNwindowType, ISW_WINDOW_TYPE_DOCK);
    ISW_ARG(&ab, IswNstrutBottom, p->phys_panel_h);
    p->shell = IswCreatePopupShell("panel", applicationShellWidgetClass,
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
    panel_tray_init(p);
    tn_net_init(p);
    tn_audio_init(p);
    tn_battery_init(p);
    tn_bt_init(p);
    tn_mount_init(p);
    clock_init(p);

    IswRealizeWidget(p->shell);
    IswMapWidget(p->shell);

    /* Claim system tray selection (needs realized window) */

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
    if (p->tray_net)
        tn_net_reload_theme(p->tray_net);
    if (p->tray_audio)
        tn_audio_reload_theme(p->tray_audio);
    if (p->tray_battery)
        tn_battery_reload_theme(p->tray_battery);
    if (p->tray_bt)
        tn_bt_reload_theme(p->tray_bt);
    if (p->tray_mount)
        tn_mount_reload_theme(p->tray_mount);
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
        //pager_reload_config(p);
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

    //* Check for screen changes (RandR) */
    panel_reconfigure(p);

    taskbar_update(p);
    taskbar_highlight_active(p);
    //pager_update(p);
//
    //if (p->launch_id) {
    //    xcb_window_t *wins = NULL;
    //    int n = isde_ewmh_get_client_list(p->ewmh, &wins);
    //    free(wins);
    //    if (n > prev_client_count) {
    //        panel_clear_launch(p);
    //    }
    //    prev_client_count = n;
    //}
//
    ///* Dismiss popups when focus moves to a managed window */
    //if (p->active_popup) {
    //    xcb_window_t active = isde_ewmh_get_active_window(p->ewmh);
    //    if (active != XCB_WINDOW_NONE && active != last_active) {
    //        panel_dismiss_popup(p);
    //    }
    //    last_active = active;
    //} else {
    //    last_active = isde_ewmh_get_active_window(p->ewmh);
    //}

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
    //if (p->active_popup == p->start_shell) {
    //    const IsdeColorScheme *s = isde_theme_current();
    //    if (s) {
    //        Pixel fg = panel_color_pixel(p, s->taskbar_button.fg);
    //        Pixel bg = panel_color_pixel(p, s->taskbar_button.bg);
    //        IswArgBuilder ab = IswArgBuilderInit();
    //        IswArgForeground(&ab, fg);
    //        IswArgBackground(&ab, bg);
    //        IswSetValues(p->start_btn, ab.args, ab.count);
    //    }
    //}

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
    isde_desktop_launch_notify(de, files, nfiles,
                               IswDisplayOf(p->toplevel), &id);
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
    tn_mount_cleanup(p);
    tn_bt_cleanup(p);
    tn_battery_cleanup(p);
    tn_audio_cleanup(p);
    tn_net_cleanup(p);
    panel_tray_cleanup(p);
    taskbar_cleanup(p);
    //pager_cleanup(p);
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
    IswDestroyApplicationContext(p->app);
}
