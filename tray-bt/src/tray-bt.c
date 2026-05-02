#define _POSIX_C_SOURCE 200809L
/*
 * tray-bt.c — system tray applet for BlueZ bluetooth management
 */
#include "tray-bt.h"

#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "isde/isde-config.h"

/* ---------- icon names per state ---------- */

static const char *icon_names[ICON_BT_COUNT] = {
    [ICON_BT_DISABLED]  = "bluetooth-disabled.svg",
    [ICON_BT_IDLE]      = "bluetooth-active.svg",
    [ICON_BT_CONNECTED] = "bluetooth-connected.svg",
};

static const char *icon_fallback_paths[ICON_BT_COUNT] = {
    [ICON_BT_DISABLED]  = "/usr/share/icons/isde-standard/status/bluetooth-disabled.svg",
    [ICON_BT_IDLE]      = "/usr/share/icons/isde-standard/status/bluetooth-active.svg",
    [ICON_BT_CONNECTED] = "/usr/share/icons/isde-standard/status/bluetooth-connected.svg",
};

/* ---------- icon state computation ---------- */

static int compute_icon_state(TrayBt *tb)
{
    if (!tb->bluez_available || !tb->has_adapter || !tb->adapter.powered)
        return ICON_BT_DISABLED;

    for (int i = 0; i < tb->ndevices; i++) {
        if (tb->devices[i].connected)
            return ICON_BT_CONNECTED;
    }

    return ICON_BT_IDLE;
}

/* ---------- icon loading ---------- */

static void load_icon(TrayBt *tb, int state)
{
    if (!tb->tray_icon)
        return;

    const char *fg_hex = NULL;
    char hex_buf[8];
    const IsdeColorScheme *scheme = isde_theme_current();
    if (scheme) {
        snprintf(hex_buf, sizeof(hex_buf), "#%06X",
                 scheme->taskbar.fg & 0xFFFFFF);
        fg_hex = hex_buf;
    }

    ISWSVGImage *svg = ISWSVGLoadFile(icon_names[state], "px", 96.0, fg_hex);
    if (!svg)
        svg = ISWSVGLoadFile(icon_fallback_paths[state], "px", 96.0, fg_hex);
    if (!svg) {
        fprintf(stderr, "isde-tray-bt: cannot load icon %s\n",
                icon_names[state]);
        return;
    }

    xcb_window_t win = IswTrayIconGetWindow(tb->tray_icon);
    xcb_connection_t *conn = IswDisplay(tb->toplevel);
    unsigned int size = 22;

    xcb_get_geometry_cookie_t gc = xcb_get_geometry(conn, win);
    xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(conn, gc, NULL);
    if (geo) {
        if (geo->width > 0)
            size = geo->width;
        free(geo);
    }

    unsigned char *rgba = ISWSVGRasterize(svg, size, size);
    if (rgba) {
        IswTrayIconSetRGBA(tb->tray_icon, rgba, size, size);
        free(rgba);
    }

    ISWSVGDestroy(svg);
}

static void deferred_icon_load(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayBt *tb = (TrayBt *)client_data;
    tb->icon_state = compute_icon_state(tb);
    load_icon(tb, tb->icon_state);
}

void tray_bt_update_icon(TrayBt *tb)
{
    int new_state = compute_icon_state(tb);
    if (new_state != tb->icon_state) {
        tb->icon_state = new_state;
        load_icon(tb, new_state);
    }
}

/* ---------- click callback ---------- */

static void on_icon_click(IswTrayIcon icon, int button, IswPointer closure)
{
    (void)icon;
    TrayBt *tb = (TrayBt *)closure;

    if (button == 1) {
        if (tb->bluez_available)
            tb_bluez_refresh(tb);
        tb_menu_show(tb);
    }
}

/* ---------- D-Bus input callbacks ---------- */

static void system_bus_input_cb(IswPointer client_data, int *fd,
                                IswInputId *id)
{
    (void)fd; (void)id;
    TrayBt *tb = (TrayBt *)client_data;

    if (!tb->system_bus)
        return;

    dbus_connection_read_write(tb->system_bus, 0);
    while (dbus_connection_dispatch(tb->system_bus) ==
           DBUS_DISPATCH_DATA_REMAINS) {
        /* drain */
    }
}

static void session_bus_input_cb(IswPointer client_data, int *fd,
                                 IswInputId *id)
{
    (void)fd; (void)id;
    TrayBt *tb = (TrayBt *)client_data;

    if (tb->session_dbus)
        isde_dbus_dispatch(tb->session_dbus);
}

static void on_settings_changed(const char *section, const char *key,
                                void *user_data)
{
    (void)key;
    TrayBt *tb = (TrayBt *)user_data;

    if (strcmp(section, "appearance") == 0) {
        tb->running = 0;
        tb->restart = 1;
    }
}

/* ---------- BlueZ availability polling ---------- */

static void bluez_retry_cb(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayBt *tb = (TrayBt *)client_data;

    if (tb->bluez_available)
        return;

    if (tb_bluez_refresh(tb) == 0) {
        tb->bluez_available = 1;
        tray_bt_update_icon(tb);
        fprintf(stderr, "isde-tray-bt: BlueZ appeared\n");
        return;
    }

    IswAppAddTimeOut(tb->app, 10000, bluez_retry_cb, tb);
}

/* ---------- public API ---------- */

int tray_bt_init(TrayBt *tb, int *argc, char **argv)
{
    memset(tb, 0, sizeof(*tb));

    tb->toplevel = IswAppInitialize(&tb->app, "IsdeTraybt",
                                     NULL, 0, argc, argv,
                                     NULL, NULL, 0);
    isde_theme_merge_xrm(tb->toplevel);
    if (!tb->toplevel) {
        fprintf(stderr, "isde-tray-bt: IswAppInitialize failed\n");
        return -1;
    }

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgWidth(&ab, 1);
    IswArgHeight(&ab, 1);
    IswArgMappedWhenManaged(&ab, False);
    IswSetValues(tb->toplevel, ab.args, ab.count);

    IswRealizeWidget(tb->toplevel);

    /* Create tray icon */
    tb->tray_icon = IswTrayIconCreate(tb->toplevel, NULL);
    if (!tb->tray_icon)
        fprintf(stderr, "isde-tray-bt: no tray manager\n");

    if (tb->tray_icon) {
        IswTrayIconAddClickCallback(tb->tray_icon, on_icon_click, tb);
        IswAppAddTimeOut(tb->app, 100, deferred_icon_load, tb);
    }

    /* Initialize popup menu */
    tb_menu_init(tb);

    /* Initialize D-Bus */
    if (tb_bluez_init(tb) != 0) {
        fprintf(stderr, "isde-tray-bt: D-Bus unavailable\n");
    }

    /* Register pairing agent */
    if (tb->system_bus)
        tb_agent_init(tb);

    /* Try initial BlueZ query */
    if (tb_bluez_refresh(tb) == 0) {
        tb->bluez_available = 1;
    } else {
        fprintf(stderr, "isde-tray-bt: BlueZ not available, will retry\n");
        IswAppAddTimeOut(tb->app, 10000, bluez_retry_cb, tb);
    }

    /* Register system bus fd with event loop */
    if (tb->system_bus) {
        int sys_fd = -1;
        dbus_connection_get_unix_fd(tb->system_bus, &sys_fd);
        if (sys_fd >= 0) {
            IswAppAddInput(tb->app, sys_fd, (IswPointer)IswInputReadMask,
                          system_bus_input_cb, tb);
        }
    }

    /* Session bus for theme changes */
    tb->session_dbus = isde_dbus_init();
    if (tb->session_dbus) {
        isde_dbus_settings_subscribe(tb->session_dbus,
                                     on_settings_changed, tb);
        int dbus_fd = isde_dbus_get_fd(tb->session_dbus);
        if (dbus_fd >= 0) {
            IswAppAddInput(tb->app, dbus_fd, (IswPointer)IswInputReadMask,
                          session_bus_input_cb, tb);
        }
    }

    tb->running = 1;
    return 0;
}

static void check_running(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayBt *tb = (TrayBt *)client_data;
    if (!tb->running)
        return;
    IswAppAddTimeOut(tb->app, 200, check_running, tb);
}

void tray_bt_run(TrayBt *tb)
{
    IswAppAddTimeOut(tb->app, 200, check_running, tb);
    while (tb->running) {
        IswAppProcessEvent(tb->app, IswIMAll);
    }
}

void tray_bt_cleanup(TrayBt *tb)
{
    tb_menu_cleanup(tb);
    tb_agent_cleanup(tb);
    tb_bluez_cleanup(tb);

    if (tb->session_dbus) {
        isde_dbus_free(tb->session_dbus);
        tb->session_dbus = NULL;
    }

    IswTrayIconDestroy(tb->tray_icon);

    if (tb->toplevel)
        IswDestroyWidget(tb->toplevel);
    IswDestroyApplicationContext(tb->app);
}
