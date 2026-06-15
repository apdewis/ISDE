#define _POSIX_C_SOURCE 200809L
/*
 * tray-bt.c — bluetooth tray module for isde-panel (BlueZ)
 */
#include "tray-bt.h"

#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- icon names per state ---------- */

static const char *icon_names[TB_ICON_COUNT] = {
    [TB_ICON_DISABLED]  = "bluetooth-disabled",
    [TB_ICON_IDLE]      = "bluetooth-active",
    [TB_ICON_CONNECTED] = "bluetooth-connected",
};

/* ---------- icon state computation ---------- */

static int compute_icon_state(TrayBt *tb)
{
    if (!tb->bluez_available || !tb->has_adapter || !tb->adapter.powered)
        return TB_ICON_DISABLED;

    for (int i = 0; i < tb->ndevices; i++) {
        if (tb->devices[i].connected)
            return TB_ICON_CONNECTED;
    }

    return TB_ICON_IDLE;
}

/* ---------- icon loading ---------- */

static void load_icon(TrayBt *tb, int state)
{
    if (!tb->icon)
        return;

    char *icon_path = isde_icon_find("status", icon_names[state]);
    if (!icon_path) {
        fprintf(stderr, "isde-panel: tray-bt: cannot find icon %s\n",
                icon_names[state]);
        return;
    }

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgImage(&ab, icon_path);
    IswArgLabel(&ab, "");
    IswSetValues(tb->icon, ab.args, ab.count);

    free(icon_path);
}

void tn_bt_update_icon(TrayBt *tb)
{
    int new_state = compute_icon_state(tb);
    if (new_state != tb->icon_state) {
        tb->icon_state = new_state;
        load_icon(tb, new_state);
    }
}

void tn_bt_reload_theme(TrayBt *tb)
{
    load_icon(tb, tb->icon_state);
}

/* ---------- click callback ---------- */

static void on_icon_click(Widget w, IswPointer client_data,
                          IswPointer call_data)
{
    (void)w; (void)call_data;
    TrayBt *tb = (TrayBt *)client_data;

    if (tb->bluez_available)
        tb_bluez_refresh(tb);
    tb_menu_show(tb);
}

/* ---------- D-Bus input callback ---------- */

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

/* ---------- BlueZ availability polling ---------- */

static void bluez_retry_cb(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayBt *tb = (TrayBt *)client_data;

    if (tb->bluez_available)
        return;

    if (tb_bluez_refresh(tb) == 0) {
        tb->bluez_available = 1;
        tn_bt_update_icon(tb);
        fprintf(stderr, "isde-panel: tray-bt: BlueZ appeared\n");
        return;
    }

    IswAppAddTimeOut(tb->panel->app, 10000, bluez_retry_cb, tb);
}

/* ---------- deferred initial icon load ---------- */

static void deferred_icon_load(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayBt *tb = (TrayBt *)client_data;
    tb->icon_state = compute_icon_state(tb);
    load_icon(tb, tb->icon_state);
}

/* ---------- public API ---------- */

void tn_bt_init(Panel *p)
{
    TrayBt *tb = calloc(1, sizeof(TrayBt));
    p->tray_bt = tb;
    tb->panel = p;

    tb->icon = panel_tray_add_icon(p, "trayBtn", commandWidgetClass);
    IswAddCallback(tb->icon, IswNcallback, on_icon_click, tb);

    tb_menu_init(tb);

    if (tb_bluez_init(tb) != 0) {
        fprintf(stderr, "isde-panel: tray-bt: D-Bus unavailable\n");
    }

    if (tb->system_bus)
        tb_agent_init(tb);

    if (tb_bluez_refresh(tb) == 0) {
        tb->bluez_available = 1;
    } else {
        fprintf(stderr, "isde-panel: tray-bt: BlueZ not available, will retry\n");
        IswAppAddTimeOut(p->app, 10000, bluez_retry_cb, tb);
    }

    if (tb->system_bus) {
        int sys_fd = -1;
        dbus_connection_get_unix_fd(tb->system_bus, &sys_fd);
        if (sys_fd >= 0) {
            IswAppAddInput(p->app, sys_fd, (IswPointer)IswInputReadMask,
                          system_bus_input_cb, tb);
        }
    }

    IswAppAddTimeOut(p->app, 100, deferred_icon_load, tb);
}

void tn_bt_cleanup(Panel *p)
{
    TrayBt *tb = p->tray_bt;
    if (!tb)
        return;

    tb_menu_cleanup(tb);
    tb_agent_cleanup(tb);
    tb_bluez_cleanup(tb);

    if (tb->icon) {
        panel_tray_remove_icon(p, tb->icon);
        tb->icon = NULL;
    }

    free(tb);
    p->tray_bt = NULL;
}
