#define _POSIX_C_SOURCE 200809L
/*
 * tray-net.c — system tray applet for ConnMan network management
 */
#include "tray-net.h"

#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "isde/isde-config.h"

/* ---------- icon names per state ---------- */

static const char *icon_names[ICON_COUNT] = {
    [ICON_DISCONNECTED]  = "network-offline.svg",
    [ICON_WIRED]         = "network-wired.svg",
    [ICON_WIFI_WEAK]     = "network-wireless-signal-weak.svg",
    [ICON_WIFI_OK]       = "network-wireless-signal-ok.svg",
    [ICON_WIFI_GOOD]     = "network-wireless-signal-good.svg",
    [ICON_WIFI_EXCELLENT] = "network-wireless-signal-excellent.svg",
};

static const char *icon_fallback_paths[ICON_COUNT] = {
    [ICON_DISCONNECTED]  = "/usr/share/icons/isde-standard/status/network-offline.svg",
    [ICON_WIRED]         = "/usr/share/icons/isde-standard/status/network-wired.svg",
    [ICON_WIFI_WEAK]     = "/usr/share/icons/isde-standard/status/network-wireless-signal-weak.svg",
    [ICON_WIFI_OK]       = "/usr/share/icons/isde-standard/status/network-wireless-signal-ok.svg",
    [ICON_WIFI_GOOD]     = "/usr/share/icons/isde-standard/status/network-wireless-signal-good.svg",
    [ICON_WIFI_EXCELLENT] = "/usr/share/icons/isde-standard/status/network-wireless-signal-excellent.svg",
};

/* ---------- icon state computation ---------- */

static int compute_icon_state(TrayNet *tn)
{
    if (!tn->connman_available)
        return ICON_DISCONNECTED;

    if (strcmp(tn->manager_state, "offline") == 0 ||
        strcmp(tn->manager_state, "idle") == 0)
        return ICON_DISCONNECTED;

    /* Find the connected/online service */
    for (int i = 0; i < tn->nservices; i++) {
        ServiceInfo *s = &tn->services[i];
        if (strcmp(s->state, "online") != 0 &&
            strcmp(s->state, "ready") != 0)
            continue;

        if (strcmp(s->type, "ethernet") == 0)
            return ICON_WIRED;

        if (strcmp(s->type, "wifi") == 0) {
            if (s->strength >= 75) return ICON_WIFI_EXCELLENT;
            if (s->strength >= 50) return ICON_WIFI_GOOD;
            if (s->strength >= 25) return ICON_WIFI_OK;
            return ICON_WIFI_WEAK;
        }

        return ICON_WIRED;
    }

    return ICON_DISCONNECTED;
}

/* ---------- icon loading ---------- */

static void load_icon(TrayNet *tn, int state)
{
    if (!tn->tray_icon)
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
        fprintf(stderr, "isde-tray-net: cannot load icon %s\n",
                icon_names[state]);
        return;
    }

    xcb_window_t win = IswTrayIconGetWindow(tn->tray_icon);
    xcb_connection_t *conn = IswDisplay(tn->toplevel);
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
        IswTrayIconSetRGBA(tn->tray_icon, rgba, size, size);
        free(rgba);
    }

    ISWSVGDestroy(svg);
}

static void deferred_icon_load(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayNet *tn = (TrayNet *)client_data;
    tn->icon_state = compute_icon_state(tn);
    load_icon(tn, tn->icon_state);
}

void tray_net_update_icon(TrayNet *tn)
{
    int new_state = compute_icon_state(tn);
    if (new_state != tn->icon_state) {
        tn->icon_state = new_state;
        load_icon(tn, new_state);
    }
}

/* ---------- click callback ---------- */

static void on_icon_click(IswTrayIcon icon, int button, IswPointer closure)
{
    (void)icon;
    TrayNet *tn = (TrayNet *)closure;

    if (button == 1) {
        if (tn->connman_available) {
            tn_connman_get_technologies(tn);
            tn_connman_get_services(tn);
        }
        tn_menu_show(tn);
    }
}

/* ---------- D-Bus input callbacks ---------- */

static void system_bus_input_cb(IswPointer client_data, int *fd,
                                IswInputId *id)
{
    (void)fd; (void)id;
    TrayNet *tn = (TrayNet *)client_data;

    if (!tn->system_bus)
        return;

    dbus_connection_read_write(tn->system_bus, 0);
    while (dbus_connection_dispatch(tn->system_bus) ==
           DBUS_DISPATCH_DATA_REMAINS) {
        /* drain */
    }
}

static void session_bus_input_cb(IswPointer client_data, int *fd,
                                 IswInputId *id)
{
    (void)fd; (void)id;
    TrayNet *tn = (TrayNet *)client_data;

    if (tn->session_dbus)
        isde_dbus_dispatch(tn->session_dbus);
}

static void on_settings_changed(const char *section, const char *key,
                                void *user_data)
{
    (void)key;
    TrayNet *tn = (TrayNet *)user_data;

    if (strcmp(section, "appearance") == 0) {
        tn->running = 0;
        tn->restart = 1;
    }
}

/* ---------- ConnMan availability polling ---------- */

static void connman_retry_cb(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayNet *tn = (TrayNet *)client_data;

    if (tn->connman_available)
        return;

    if (tn_connman_refresh(tn) == 0) {
        tn->connman_available = 1;
        tray_net_update_icon(tn);
        fprintf(stderr, "isde-tray-net: ConnMan appeared\n");
        return;
    }

    IswAppAddTimeOut(tn->app, 10000, connman_retry_cb, tn);
}

/* ---------- public API ---------- */

int tray_net_init(TrayNet *tn, int *argc, char **argv)
{
    memset(tn, 0, sizeof(*tn));
    snprintf(tn->manager_state, sizeof(tn->manager_state), "idle");

    tn->toplevel = IswAppInitialize(&tn->app, "IsdeTraynet",
                                     NULL, 0, argc, argv,
                                     NULL, NULL, 0);
    isde_theme_merge_xrm(tn->toplevel);
    if (!tn->toplevel) {
        fprintf(stderr, "isde-tray-net: IswAppInitialize failed\n");
        return -1;
    }

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgWidth(&ab, 1);
    IswArgHeight(&ab, 1);
    IswArgMappedWhenManaged(&ab, False);
    IswSetValues(tn->toplevel, ab.args, ab.count);

    IswRealizeWidget(tn->toplevel);

    /* Create tray icon */
    tn->tray_icon = IswTrayIconCreate(tn->toplevel, NULL);
    if (!tn->tray_icon)
        fprintf(stderr, "isde-tray-net: no tray manager\n");

    if (tn->tray_icon) {
        IswTrayIconAddClickCallback(tn->tray_icon, on_icon_click, tn);
        IswAppAddTimeOut(tn->app, 100, deferred_icon_load, tn);
    }

    /* Initialize popup menu */
    tn_menu_init(tn);

    /* Initialize D-Bus */
    if (tn_connman_init(tn) != 0) {
        fprintf(stderr, "isde-tray-net: D-Bus unavailable\n");
    }

    /* Register agent for passphrase prompts */
    if (tn->system_bus)
        tn_agent_init(tn);

    /* Try initial ConnMan query */
    if (tn_connman_refresh(tn) == 0) {
        tn->connman_available = 1;
    } else {
        fprintf(stderr, "isde-tray-net: ConnMan not available, will retry\n");
        IswAppAddTimeOut(tn->app, 10000, connman_retry_cb, tn);
    }

    /* Register system bus fd with event loop */
    if (tn->system_bus) {
        int sys_fd = -1;
        dbus_connection_get_unix_fd(tn->system_bus, &sys_fd);
        if (sys_fd >= 0) {
            IswAppAddInput(tn->app, sys_fd, (IswPointer)IswInputReadMask,
                          system_bus_input_cb, tn);
        }
    }

    /* Session bus for theme changes */
    tn->session_dbus = isde_dbus_init();
    if (tn->session_dbus) {
        isde_dbus_settings_subscribe(tn->session_dbus,
                                     on_settings_changed, tn);
        int dbus_fd = isde_dbus_get_fd(tn->session_dbus);
        if (dbus_fd >= 0) {
            IswAppAddInput(tn->app, dbus_fd, (IswPointer)IswInputReadMask,
                          session_bus_input_cb, tn);
        }
    }

    tn->running = 1;
    return 0;
}

static void check_running(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayNet *tn = (TrayNet *)client_data;
    if (!tn->running)
        return;
    IswAppAddTimeOut(tn->app, 200, check_running, tn);
}

void tray_net_run(TrayNet *tn)
{
    IswAppAddTimeOut(tn->app, 200, check_running, tn);
    while (tn->running) {
        IswAppProcessEvent(tn->app, IswIMAll);
    }
}

void tray_net_cleanup(TrayNet *tn)
{
    tn_menu_cleanup(tn);
    tn_agent_cleanup(tn);
    tn_connman_cleanup(tn);

    if (tn->session_dbus) {
        isde_dbus_free(tn->session_dbus);
        tn->session_dbus = NULL;
    }

    IswTrayIconDestroy(tn->tray_icon);

    if (tn->toplevel)
        IswDestroyWidget(tn->toplevel);
}
