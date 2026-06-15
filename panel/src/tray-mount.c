#define _POSIX_C_SOURCE 200809L
/*
 * tray-mount.c — removable disk tray module for isde-panel
 */
#include "tray-mount.h"

#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- icon loading ---------- */

static void load_icon(TrayMount *tm)
{
    if (!tm->icon)
        return;

    char *icon_path = isde_icon_find("devices", "drive-removable-media");
    if (!icon_path) {
        fprintf(stderr, "isde-panel: tray-mount: cannot find icon\n");
        return;
    }

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgImage(&ab, icon_path);
    IswArgLabel(&ab, "");
    IswSetValues(tm->icon, ab.args, ab.count);

    free(icon_path);
}

void tn_mount_reload_theme(TrayMount *tm)
{
    load_icon(tm);
}

/* ---------- click callback ---------- */

static void on_icon_click(Widget w, IswPointer client_data,
                          IswPointer call_data)
{
    (void)w; (void)call_data;
    TrayMount *tm = (TrayMount *)client_data;

    tm_dbus_list_devices(tm);
    tm_popup_show(tm);
}

/* ---------- D-Bus input callback ---------- */

static void system_bus_input_cb(IswPointer client_data, int *fd,
                                IswInputId *id)
{
    (void)fd; (void)id;
    TrayMount *tm = (TrayMount *)client_data;

    if (!tm->system_bus)
        return;

    dbus_connection_read_write(tm->system_bus, 0);
    while (dbus_connection_dispatch(tm->system_bus) ==
           DBUS_DISPATCH_DATA_REMAINS) {
        /* drain */
    }
}

/* ---------- deferred initial icon load ---------- */

static void deferred_icon_load(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayMount *tm = (TrayMount *)client_data;
    load_icon(tm);
}

/* ---------- public API ---------- */

void tn_mount_init(Panel *p)
{
    TrayMount *tm = calloc(1, sizeof(TrayMount));
    p->tray_mount = tm;
    tm->panel = p;

    tm->icon = panel_tray_add_icon(p, "trayBtn", commandWidgetClass);
    IswAddCallback(tm->icon, IswNcallback, on_icon_click, tm);

    tm_popup_init(tm);

    if (tm_dbus_init(tm) != 0) {
        fprintf(stderr, "isde-panel: tray-mount: D-Bus unavailable\n");
    }

    tm_dbus_list_devices(tm);

    if (tm->system_bus) {
        int sys_fd = -1;
        dbus_connection_get_unix_fd(tm->system_bus, &sys_fd);
        if (sys_fd >= 0) {
            IswAppAddInput(p->app, sys_fd, (IswPointer)IswInputReadMask,
                          system_bus_input_cb, tm);
        }
    }

    IswAppAddTimeOut(p->app, 100, deferred_icon_load, tm);
}

void tn_mount_cleanup(Panel *p)
{
    TrayMount *tm = p->tray_mount;
    if (!tm)
        return;

    tm_password_dialog_cleanup(tm);
    tm_popup_cleanup(tm);
    tm_dbus_cleanup(tm);

    if (tm->icon) {
        panel_tray_remove_icon(p, tm->icon);
        tm->icon = NULL;
    }

    free(tm);
    p->tray_mount = NULL;
}
