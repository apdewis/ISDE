#define _POSIX_C_SOURCE 200809L
/*
 * tray-net.c — network tray module for isde-panel (ConnMan)
 */
#include "tray-net.h"

#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- icon names per state ---------- */

static const char *icon_names[TN_ICON_COUNT] = {
    [TN_ICON_DISCONNECTED]   = "network-offline",
    [TN_ICON_WIRED]          = "network-wired",
    [TN_ICON_WIFI_WEAK]      = "network-wireless-signal-weak",
    [TN_ICON_WIFI_OK]        = "network-wireless-signal-ok",
    [TN_ICON_WIFI_GOOD]      = "network-wireless-signal-good",
    [TN_ICON_WIFI_EXCELLENT] = "network-wireless-signal-excellent",
};

/* ---------- icon state computation ---------- */

static int compute_icon_state(TrayNet *tn)
{
    if (!tn->connman_available)
        return TN_ICON_DISCONNECTED;

    if (strcmp(tn->manager_state, "offline") == 0 ||
        strcmp(tn->manager_state, "idle") == 0)
        return TN_ICON_DISCONNECTED;

    for (int i = 0; i < tn->nservices; i++) {
        ServiceInfo *s = &tn->services[i];
        if (strcmp(s->state, "online") != 0 &&
            strcmp(s->state, "ready") != 0)
            continue;

        if (strcmp(s->type, "ethernet") == 0)
            return TN_ICON_WIRED;

        if (strcmp(s->type, "wifi") == 0) {
            if (s->strength >= 75) return TN_ICON_WIFI_EXCELLENT;
            if (s->strength >= 50) return TN_ICON_WIFI_GOOD;
            if (s->strength >= 25) return TN_ICON_WIFI_OK;
            return TN_ICON_WIFI_WEAK;
        }

        return TN_ICON_WIRED;
    }

    return TN_ICON_DISCONNECTED;
}

/* ---------- icon loading ---------- */

static void load_icon(TrayNet *tn, int state)
{
    if (!tn->icon)
        return;

    char *icon_path = isde_icon_find("status", icon_names[state]);
    if (!icon_path) {
        fprintf(stderr, "isde-panel: tray-net: cannot find icon %s\n",
                icon_names[state]);
        return;
    }

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgImage(&ab, icon_path);
    IswArgLabel(&ab, "");
    IswSetValues(tn->icon, ab.args, ab.count);

    free(icon_path);
}

void tn_net_update_icon(TrayNet *tn)
{
    int new_state = compute_icon_state(tn);
    if (new_state != tn->icon_state) {
        tn->icon_state = new_state;
        load_icon(tn, new_state);
    }
}

void tn_net_reload_theme(TrayNet *tn)
{
    load_icon(tn, tn->icon_state);
}

/* ---------- click callback ---------- */

static void on_icon_click(Widget w, IswPointer client_data,
                          IswPointer call_data)
{
    (void)w; (void)call_data;
    TrayNet *tn = (TrayNet *)client_data;

    if (tn->connman_available) {
        tn_connman_get_technologies(tn);
        tn_connman_get_services(tn);
    }
    tn_menu_show(tn);
}

/* ---------- D-Bus input callback ---------- */

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

/* ---------- ConnMan availability polling ---------- */

static void connman_retry_cb(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayNet *tn = (TrayNet *)client_data;

    if (tn->connman_available)
        return;

    if (tn_connman_refresh(tn) == 0) {
        tn->connman_available = 1;
        tn_net_update_icon(tn);
        fprintf(stderr, "isde-panel: tray-net: ConnMan appeared\n");
        return;
    }

    IswAppAddTimeOut(tn->panel->app, 10000, connman_retry_cb, tn);
}

/* ---------- deferred initial icon load ---------- */

static void deferred_icon_load(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayNet *tn = (TrayNet *)client_data;
    tn->icon_state = compute_icon_state(tn);
    load_icon(tn, tn->icon_state);
}

/* ---------- public API ---------- */

void tn_net_init(Panel *p)
{
    TrayNet *tn = calloc(1, sizeof(TrayNet));
    p->tray_net = tn;
    tn->panel = p;
    snprintf(tn->manager_state, sizeof(tn->manager_state), "idle");

    tn->icon = panel_tray_add_icon(p, "trayBtn", commandWidgetClass);
    IswAddCallback(tn->icon, IswNcallback, on_icon_click, tn);

    tn_menu_init(tn);

    if (tn_connman_init(tn) != 0) {
        fprintf(stderr, "isde-panel: tray-net: D-Bus unavailable\n");
    }

    if (tn->system_bus)
        tn_agent_init(tn);

    if (tn_connman_refresh(tn) == 0) {
        tn->connman_available = 1;
    } else {
        fprintf(stderr, "isde-panel: tray-net: ConnMan not available, will retry\n");
        IswAppAddTimeOut(p->app, 10000, connman_retry_cb, tn);
    }

    if (tn->system_bus) {
        int sys_fd = -1;
        dbus_connection_get_unix_fd(tn->system_bus, &sys_fd);
        if (sys_fd >= 0) {
            IswAppAddInput(p->app, sys_fd, (IswPointer)IswInputReadMask,
                          system_bus_input_cb, tn);
        }
    }

    IswAppAddTimeOut(p->app, 100, deferred_icon_load, tn);
}

void tn_net_cleanup(Panel *p)
{
    TrayNet *tn = p->tray_net;
    if (!tn)
        return;

    tn_menu_cleanup(tn);
    tn_agent_cleanup(tn);
    tn_connman_cleanup(tn);

    if (tn->icon) {
        panel_tray_remove_icon(p, tn->icon);
        tn->icon = NULL;
    }

    free(tn);
    p->tray_net = NULL;
}
