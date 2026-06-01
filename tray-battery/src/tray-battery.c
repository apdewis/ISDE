#define _POSIX_C_SOURCE 200809L
/*
 * tray-battery.c — system tray applet for battery status
 */
#include "tray-battery.h"

#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "isde/isde-xdg.h"

/* ---------- icon loading ---------- */

static const char *icon_name_for_state(int capacity, int charging)
{
    if (charging) {
        if (capacity >= 80) { return "battery-full-charging"; }
        if (capacity >= 50) { return "battery-good-charging"; }
        if (capacity >= 20) { return "battery-low-charging"; }
        return "battery-caution-charging";
    }
    if (capacity >= 80) { return "battery-full"; }
    if (capacity >= 50) { return "battery-good"; }
    if (capacity >= 20) { return "battery-low"; }
    return "battery-caution";
}

static int compute_icon_state(TrayBattery *tb)
{
    const IsdeBattery *bat = isde_power_get_battery(tb->power, 0);
    if (!bat) {
        return -1;
    }
    int charging = (bat->status == ISDE_BAT_CHARGING) ? 1 : 0;
    /* Encode: capacity in low bits, charging flag in bit 8 */
    return (bat->capacity & 0xFF) | (charging << 8);
}

static void load_tray_icon(TrayBattery *tb)
{
    const IsdeBattery *bat = isde_power_get_battery(tb->power, 0);
    int capacity = bat ? bat->capacity : 0;
    int charging = bat ? (bat->status == ISDE_BAT_CHARGING) : 0;
    const char *name = icon_name_for_state(capacity, charging);

    const char *fg_hex = NULL;
    char hex_buf[8];
    const IsdeColorScheme *scheme = isde_theme_current();
    if (scheme) {
        snprintf(hex_buf, sizeof(hex_buf), "#%06X",
                 scheme->taskbar.fg & 0xFFFFFF);
        fg_hex = hex_buf;
    }

    char *icon_path = isde_icon_find("status", name);
    ISWSVGImage *svg = icon_path
        ? ISWSVGLoadFile(icon_path, "px", 96.0, fg_hex) : NULL;
    free(icon_path);
    if (!svg) {
        fprintf(stderr, "isde-tray-battery: cannot load icon %s\n", name);
        return;
    }

    xcb_window_t win = IswTrayIconGetWindow(tb->tray_icon);
    xcb_connection_t *conn = IswDisplay(tb->toplevel);
    unsigned int size = 22;

    xcb_get_geometry_cookie_t gc = xcb_get_geometry(conn, win);
    xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(conn, gc, NULL);
    if (geo) {
        if (geo->width > 0) {
            size = geo->width;
        }
        free(geo);
    }

    unsigned char *rgba = ISWSVGRasterize(svg, size, size);
    if (rgba) {
        IswTrayIconSetRGBA(tb->tray_icon, rgba, size, size);
        free(rgba);
    }

    ISWSVGDestroy(svg);
    tb->icon_state = compute_icon_state(tb);
}

static void deferred_icon_load(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayBattery *tb = (TrayBattery *)client_data;
    load_tray_icon(tb);
}

void tray_battery_update_icon(TrayBattery *tb)
{
    if (!tb->tray_icon) {
        return;
    }

    int new_state = compute_icon_state(tb);
    if (new_state != tb->icon_state) {
        load_tray_icon(tb);
    }
}

/* Re-rasterise the icon when the tray manager resizes our window, so it
 * stays crisp across the initial dock and any panel-height/DPI change. */
static void on_icon_resize(IswTrayIcon icon, unsigned int w, unsigned int h,
                           IswPointer closure)
{
    (void)icon; (void)w; (void)h;
    TrayBattery *tb = (TrayBattery *)closure;
    load_tray_icon(tb);
}

/* ---------- poll timer ---------- */

static void poll_timer_cb(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayBattery *tb = (TrayBattery *)client_data;
    if (!tb->running) {
        return;
    }

    isde_power_poll(tb->power);
    tray_battery_update_icon(tb);
    tb_popup_update(tb);

    tb->poll_timer = IswAppAddTimeOut(tb->app, 5000, poll_timer_cb, tb);
}

/* ---------- click callback ---------- */

static void on_icon_click(IswTrayIcon icon, int button, IswPointer closure)
{
    (void)icon;
    TrayBattery *tb = (TrayBattery *)closure;

    switch (button) {
    case 1:
        tb_popup_show(tb);
        break;
    case 3:
        tb_menu_show(tb);
        break;
    }
}

/* ---------- D-Bus input callbacks ---------- */

static void on_theme_changed(void *user_data)
{
    TrayBattery *tb = (TrayBattery *)user_data;
    IswReloadResources(tb->toplevel);

    const IsdeColorScheme *s = isde_theme_current();
    if (s && tb->tray_icon) {
        xcb_connection_t *conn = IswDisplay(tb->toplevel);
        xcb_window_t win = IswTrayIconGetWindow(tb->tray_icon);
        xcb_alloc_color_reply_t *acr = xcb_alloc_color_reply(conn,
            xcb_alloc_color(conn, IswScreen(tb->toplevel)->default_colormap,
                            ((s->taskbar.bg >> 16) & 0xFF) * 257,
                            ((s->taskbar.bg >>  8) & 0xFF) * 257,
                            ( s->taskbar.bg        & 0xFF) * 257), NULL);
        if (acr) {
            uint32_t bg = acr->pixel;
            free(acr);
            xcb_change_window_attributes(conn, win, XCB_CW_BACK_PIXEL, &bg);
            xcb_clear_area(conn, 0, win, 0, 0, 0, 0);
        }
    }
    load_tray_icon(tb);
}

static void session_bus_input_cb(IswPointer client_data, int *fd,
                                 IswInputId *id)
{
    (void)fd; (void)id;
    TrayBattery *tb = (TrayBattery *)client_data;

    if (tb->session_dbus) {
        isde_dbus_dispatch(tb->session_dbus);
    }
}


/* ---------- public API ---------- */

int tray_battery_init(TrayBattery *tb, int *argc, char **argv)
{
    memset(tb, 0, sizeof(*tb));

    tb->power = isde_power_init();
    if (!tb->power || isde_power_battery_count(tb->power) == 0) {
        fprintf(stderr, "isde-tray-battery: no batteries found\n");
        isde_power_free(tb->power);
        tb->power = NULL;
        return -1;
    }

    tb->toplevel = IswAppInitialize(&tb->app, "IsdeTrayBattery",
                                     NULL, 0, argc, argv,
                                     NULL, NULL, 0);
    isde_theme_merge_xrm(tb->toplevel);
    if (!tb->toplevel) {
        fprintf(stderr, "isde-tray-battery: IswAppInitialize failed\n");
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
    if (!tb->tray_icon) {
        fprintf(stderr, "isde-tray-battery: no tray manager\n");
    }

    if (tb->tray_icon) {
        IswTrayIconAddClickCallback(tb->tray_icon, on_icon_click, tb);
        IswTrayIconSetResizeCallback(tb->tray_icon, on_icon_resize, tb);
        IswAppAddTimeOut(tb->app, 100, deferred_icon_load, tb);
    }

    /* Initialize popup and menu */
    tb_popup_init(tb);
    tb_menu_init(tb);

    /* Session D-Bus for theme changes */
    tb->session_dbus = isde_dbus_init();
    if (tb->session_dbus) {
        isde_theme_watch(tb->session_dbus, tb->toplevel,
                         on_theme_changed, tb);
        int dbus_fd = isde_dbus_get_fd(tb->session_dbus);
        if (dbus_fd >= 0) {
            IswAppAddInput(tb->app, dbus_fd, (IswPointer)IswInputReadMask,
                          session_bus_input_cb, tb);
        }
    }

    /* Load small font */
    {
        const char *fam = "Sans";
        int sz = 9;
        char errbuf[256];
        IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf,
                                                sizeof(errbuf));
        if (cfg) {
            IsdeConfigTable *root = isde_config_root(cfg);
            IsdeConfigTable *fonts = isde_config_table(root, "fonts");
            if (fonts) {
                fam = isde_config_string(fonts, "small_family", fam);
                int csz = (int)isde_config_int(fonts, "small_size", 0);
                if (csz > 0) { sz = csz; }
            }
        }
        char spec[128];
        snprintf(spec, sizeof(spec), "%s-%d", fam, sz);
        tb->small_font = isde_resolve_font(tb->toplevel, spec);
        if (cfg) { isde_config_free(cfg); }
    }

    tb->icon_state = -1;
    tb->running = 1;

    /* Start poll timer */
    tb->poll_timer = IswAppAddTimeOut(tb->app, 5000, poll_timer_cb, tb);

    return 0;
}

static void check_running(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayBattery *tb = (TrayBattery *)client_data;
    if (!tb->running) {
        return;
    }
    IswAppAddTimeOut(tb->app, 200, check_running, tb);
}

void tray_battery_run(TrayBattery *tb)
{
    IswAppAddTimeOut(tb->app, 200, check_running, tb);
    while (tb->running) {
        IswAppProcessEvent(tb->app, IswIMAll);
    }
}

void tray_battery_cleanup(TrayBattery *tb)
{
    tb_popup_cleanup(tb);
    tb_menu_cleanup(tb);

    if (tb->session_dbus) {
        isde_dbus_free(tb->session_dbus);
        tb->session_dbus = NULL;
    }

    isde_power_free(tb->power);
    tb->power = NULL;

    IswTrayIconDestroy(tb->tray_icon);

    if (tb->toplevel) {
        IswDestroyWidget(tb->toplevel);
    }
    IswDestroyApplicationContext(tb->app);
}
