#define _POSIX_C_SOURCE 200809L
/*
 * tray-audio.c — system tray applet for audio volume control
 *
 * Uses IswTrayIcon to embed a speaker icon in the panel's system tray.
 * Left-click shows a tabbed popup with volume sliders.
 * Right-click shows a sink selection menu.
 * Scroll wheel adjusts the default sink volume.
 */
#include "tray-audio.h"

#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- icon loading ---------- */

static const char *icon_name_for_state(int state)
{
    switch (state) {
    case 0:  return "audio-volume-muted";
    case 1:  return "audio-volume-low";
    case 2:  return "audio-volume-medium";
    case 3:  return "audio-volume-high";
    default: return "audio-volume-muted";
    }
}

static int compute_icon_state(TrayAudio *ta)
{
    SinkInfo *def = ta_default_sink(ta);
    if (!def || def->muted || def->volume <= 0.0f)
        return 0;
    if (def->volume <= 0.33f)
        return 1;
    if (def->volume <= 0.66f)
        return 2;
    return 3;
}

static void load_tray_icon(TrayAudio *ta)
{
    int state = compute_icon_state(ta);
    const char *name = icon_name_for_state(state);

    /* Resolve currentColor to the taskbar foreground from the theme */
    const char *fg_hex = NULL;
    char hex_buf[8];
    const IsdeColorScheme *scheme = isde_theme_current();
    if (scheme) {
        snprintf(hex_buf, sizeof(hex_buf), "#%06X",
                 scheme->taskbar.fg & 0xFFFFFF);
        fg_hex = hex_buf;
    }

    /* Try icon theme lookup first, then direct filename */
    char svg_file[512];
    snprintf(svg_file, sizeof(svg_file), "%s.svg", name);

    ISWSVGImage *svg = ISWSVGLoadFile(svg_file, "px", 96.0, fg_hex);
    if (!svg) {
        char path[1024];
        snprintf(path, sizeof(path),
                 "/usr/share/icons/isde-standard/status/%s.svg", name);
        svg = ISWSVGLoadFile(path, "px", 96.0, fg_hex);
    }
    if (!svg) {
        fprintf(stderr, "isde-tray-audio: cannot load icon %s\n", name);
        return;
    }

    /* Rasterize at the icon window's physical size */
    xcb_window_t win = IswTrayIconGetWindow(ta->tray_icon);
    xcb_connection_t *conn = IswDisplay(ta->toplevel);
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
        IswTrayIconSetRGBA(ta->tray_icon, rgba, size, size);
        free(rgba);
    }

    ISWSVGDestroy(svg);
    ta->icon_state = state;
}

static void deferred_icon_load(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayAudio *ta = (TrayAudio *)client_data;
    load_tray_icon(ta);
}

void tray_audio_update_icon(TrayAudio *ta)
{
    if (!ta->tray_icon)
        return;

    int new_state = compute_icon_state(ta);
    if (new_state != ta->icon_state)
        load_tray_icon(ta);
}

/* ---------- click callback ---------- */

static void on_icon_click(IswTrayIcon icon, int button, IswPointer closure)
{
    (void)icon;
    TrayAudio *ta = (TrayAudio *)closure;

    switch (button) {
    case 1:  /* left click — toggle popup */
        ta_popup_show(ta);
        break;

    case 3:  /* right click — sink selection menu */
        ta_menu_show(ta);
        break;

    case 4:  /* scroll up — volume +5% */
    case 5: { /* scroll down — volume -5% */
        SinkInfo *def = ta_default_sink(ta);
        if (def) {
            float delta = (button == 4) ? 0.05f : -0.05f;
            float vol = def->volume + delta;
            if (vol < 0.0f) vol = 0.0f;
            if (vol > 1.0f) vol = 1.0f;
            ta_pw_set_volume(ta, def->id, vol);
        }
        break;
    }
    }
}

/* ---------- D-Bus input callbacks ---------- */

static void session_bus_input_cb(IswPointer client_data, int *fd,
                                 IswInputId *id)
{
    (void)fd; (void)id;
    TrayAudio *ta = (TrayAudio *)client_data;

    if (ta->session_dbus)
        isde_dbus_dispatch(ta->session_dbus);
}

static void on_settings_changed(const char *section, const char *key,
                                void *user_data)
{
    (void)key;
    TrayAudio *ta = (TrayAudio *)user_data;

    if (strcmp(section, "appearance") == 0) {
        ta->running = 0;
        ta->restart = 1;
    }
}

/* ---------- public API ---------- */

int tray_audio_init(TrayAudio *ta, int *argc, char **argv)
{
    memset(ta, 0, sizeof(*ta));

    ta->toplevel = IswAppInitialize(&ta->app, "IsdeTrayaudio",
                                     NULL, 0, argc, argv,
                                     NULL, NULL, 0);
    isde_theme_merge_xrm(ta->toplevel);
    if (!ta->toplevel) {
        fprintf(stderr, "isde-tray-audio: IswAppInitialize failed\n");
        return -1;
    }

    /* Invisible toplevel — connection provider only */
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgWidth(&ab, 1);
    IswArgHeight(&ab, 1);
    IswArgMappedWhenManaged(&ab, False);
    IswSetValues(ta->toplevel, ab.args, ab.count);

    IswRealizeWidget(ta->toplevel);

    /* Create tray icon */
    ta->tray_icon = IswTrayIconCreate(ta->toplevel, NULL);
    if (!ta->tray_icon) {
        fprintf(stderr, "isde-tray-audio: no tray manager\n");
    }

    if (ta->tray_icon) {
        IswTrayIconAddClickCallback(ta->tray_icon, on_icon_click, ta);
        IswAppAddTimeOut(ta->app, 100, deferred_icon_load, ta);
    }

    /* Initialize popup and menu */
    ta_popup_init(ta);
    ta_menu_init(ta);

    /* Initialize PipeWire */
    if (ta_pw_init(ta) != 0) {
        fprintf(stderr, "isde-tray-audio: PipeWire unavailable\n");
    }

    /* Session D-Bus for theme changes */
    ta->session_dbus = isde_dbus_init();
    if (ta->session_dbus) {
        isde_dbus_settings_subscribe(ta->session_dbus,
                                     on_settings_changed, ta);
        int dbus_fd = isde_dbus_get_fd(ta->session_dbus);
        if (dbus_fd >= 0) {
            IswAppAddInput(ta->app, dbus_fd, (IswPointer)IswInputReadMask,
                          session_bus_input_cb, ta);
        }
    }

    /* Load the system small font (from [fonts] config, default 9pt) */
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
                if (csz > 0) sz = csz;
            }
        }
        char spec[128];
        snprintf(spec, sizeof(spec), "%s-%d", fam, sz);
        ta->small_font = isde_resolve_font(ta->toplevel, spec);
        if (cfg) isde_config_free(cfg);
    }

    ta->icon_state = -1;  /* Force icon load on first update */
    ta->running = 1;
    return 0;
}

static void check_running(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayAudio *ta = (TrayAudio *)client_data;
    if (!ta->running)
        return;
    IswAppAddTimeOut(ta->app, 200, check_running, ta);
}

void tray_audio_run(TrayAudio *ta)
{
    IswAppAddTimeOut(ta->app, 200, check_running, ta);
    while (ta->running) {
        IswAppProcessEvent(ta->app, IswIMAll);
    }
}

void tray_audio_cleanup(TrayAudio *ta)
{
    ta_popup_cleanup(ta);
    ta_menu_cleanup(ta);
    ta_pw_cleanup(ta);

    if (ta->session_dbus) {
        isde_dbus_free(ta->session_dbus);
        ta->session_dbus = NULL;
    }

    IswTrayIconDestroy(ta->tray_icon);

    if (ta->toplevel)
        IswDestroyWidget(ta->toplevel);
}
