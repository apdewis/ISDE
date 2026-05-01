#define _POSIX_C_SOURCE 200809L
/*
 * tray-mount.c — system tray applet for removable disk management
 *
 * Uses IswTrayIcon to embed an icon in the panel's system tray.
 * Click shows a popup menu listing devices with mount/unmount/eject.
 */
#include "tray-mount.h"

#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "isde/isde-config.h"

/* ---------- icon loading ---------- */

static void load_tray_icon(TrayMount *tm)
{
    /* Resolve currentColor to the taskbar foreground from the theme */
    const char *fg_hex = NULL;
    char hex_buf[8];
    const IsdeColorScheme *scheme = isde_theme_current();
    if (scheme) {
        snprintf(hex_buf, sizeof(hex_buf), "#%06X", scheme->taskbar.fg & 0xFFFFFF);
        fg_hex = hex_buf;
    }

    ISWSVGImage *svg = ISWSVGLoadFile(
        "drive-removable-media.svg", "px", 96.0, fg_hex);
    if (!svg) {
        /* Try absolute path as fallback */
        svg = ISWSVGLoadFile(
            "/usr/share/icons/isde-standard/devices/drive-removable-media.svg",
            "px", 96.0, fg_hex);
    }
    if (!svg) {
        fprintf(stderr, "isde-tray-mount: cannot load icon\n");
        return;
    }

    /* Rasterize at the icon window's physical size for crisp rendering */
    xcb_window_t win = IswTrayIconGetWindow(tm->tray_icon);
    xcb_connection_t *conn = IswDisplay(tm->toplevel);
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
        IswTrayIconSetRGBA(tm->tray_icon, rgba, size, size);
        free(rgba);
    }

    ISWSVGDestroy(svg);
}

static void deferred_icon_load(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayMount *tm = (TrayMount *)client_data;
    load_tray_icon(tm);
}

/* ---------- click callback ---------- */

static void on_icon_click(IswTrayIcon icon, int button, IswPointer closure)
{
    (void)icon;
    TrayMount *tm = (TrayMount *)closure;

    if (button == 1) {
        tm_dbus_list_devices(tm);
        tm_popup_show(tm);
    }
}

/* ---------- D-Bus input callbacks ---------- */

static void system_bus_input_cb(IswPointer client_data, int *fd,
                                IswInputId *id)
{
    (void)fd; (void)id;
    TrayMount *tm = (TrayMount *)client_data;

    if (!tm->system_bus) {
        return;
    }

    dbus_connection_read_write(tm->system_bus, 0);
    while (dbus_connection_dispatch(tm->system_bus) ==
           DBUS_DISPATCH_DATA_REMAINS) {
        /* drain */
    }
}

static void session_bus_input_cb(IswPointer client_data, int *fd,
                                 IswInputId *id)
{
    (void)fd; (void)id;
    TrayMount *tm = (TrayMount *)client_data;

    if (tm->session_dbus) {
        isde_dbus_dispatch(tm->session_dbus);
    }
}

static void on_settings_changed(const char *section, const char *key,
                                void *user_data)
{
    (void)key;
    TrayMount *tm = (TrayMount *)user_data;

    if (strcmp(section, "appearance") == 0) {
        tm->running = 0;
        tm->restart = 1;
    }
}

/* ---------- public API ---------- */

int tray_mount_init(TrayMount *tm, int *argc, char **argv)
{
    memset(tm, 0, sizeof(*tm));

    tm->toplevel = IswAppInitialize(&tm->app, "IsdeTraymount",
                                     NULL, 0, argc, argv,
                                     NULL, NULL, 0);
    isde_theme_merge_xrm(tm->toplevel);
    if (!tm->toplevel) {
        fprintf(stderr, "isde-tray-mount: IswAppInitialize failed\n");
        return -1;
    }

    /* Give the toplevel a nominal size and suppress mapping —
     * it exists only to provide a connection for IswTrayIconCreate
     * and as the parent for the popup menu. */
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgWidth(&ab, 1);
    IswArgHeight(&ab, 1);
    IswArgMappedWhenManaged(&ab, False);
    IswSetValues(tm->toplevel, ab.args, ab.count);

    IswRealizeWidget(tm->toplevel);

    /* Create tray icon */
    tm->tray_icon = IswTrayIconCreate(tm->toplevel, NULL);
    if (!tm->tray_icon) {
        fprintf(stderr, "isde-tray-mount: no tray manager, retrying later\n");
        /* Could set up a timer to retry, but for now just continue
         * without a visible icon — D-Bus signals will still work */
    }

    /* Load icon after a short delay so the panel has time to dock
     * and resize the icon window — we need the final physical size
     * to rasterize the SVG crisply. */
    if (tm->tray_icon) {
        IswTrayIconAddClickCallback(tm->tray_icon, on_icon_click, tm);
        IswAppAddTimeOut(tm->app, 100, deferred_icon_load, tm);
    }

    /* Initialize popup menu */
    tm_popup_init(tm);


    /* Initialize D-Bus */
    if (tm_dbus_init(tm) != 0) {
        fprintf(stderr, "isde-tray-mount: D-Bus unavailable\n");
    }

    /* Get initial device list */
    tm_dbus_list_devices(tm);

    /* Register D-Bus fds with event loop */
    if (tm->system_bus) {
        int sys_fd = -1;
        dbus_connection_get_unix_fd(tm->system_bus, &sys_fd);
        if (sys_fd >= 0) {
            IswAppAddInput(tm->app, sys_fd, (IswPointer)IswInputReadMask,
                          system_bus_input_cb, tm);
        }
    }

    tm->session_dbus = isde_dbus_init();
    if (tm->session_dbus) {
        isde_dbus_settings_subscribe(tm->session_dbus,
                                     on_settings_changed, tm);
        int dbus_fd = isde_dbus_get_fd(tm->session_dbus);
        if (dbus_fd >= 0) {
            IswAppAddInput(tm->app, dbus_fd, (IswPointer)IswInputReadMask,
                          session_bus_input_cb, tm);
        }
    }

    tm->running = 1;
    return 0;
}

static void check_running(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayMount *tm = (TrayMount *)client_data;
    if (!tm->running) {
        return;  /* timer won't be re-added, app loop will drain and exit */
    }
    IswAppAddTimeOut(tm->app, 200, check_running, tm);
}

void tray_mount_run(TrayMount *tm)
{
    IswAppAddTimeOut(tm->app, 200, check_running, tm);
    while (tm->running) {
        IswAppProcessEvent(tm->app, IswIMAll);
    }
}

void tray_mount_cleanup(TrayMount *tm)
{
    tm_popup_cleanup(tm);
    tm_dbus_cleanup(tm);

    if (tm->session_dbus) {
        isde_dbus_free(tm->session_dbus);
        tm->session_dbus = NULL;
    }

    IswTrayIconDestroy(tm->tray_icon);

    if (tm->toplevel)
        IswDestroyWidget(tm->toplevel);
    IswDestroyApplicationContext(tm->app);
}
