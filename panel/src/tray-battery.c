#define _POSIX_C_SOURCE 200809L
/*
 * tray-battery.c — battery tray module for isde-panel
 */
#include "tray-battery.h"

#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    if (!tb->power) {
        return -1;
    }
    const IsdeBattery *bat = isde_power_get_battery(tb->power, 0);
    if (!bat) {
        return -1;
    }
    int charging = (bat->status == ISDE_BAT_CHARGING) ? 1 : 0;
    /* Encode: capacity in low bits, charging flag in bit 8 */
    return (bat->capacity & 0xFF) | (charging << 8);
}

static void load_icon(TrayBattery *tb, int state)
{
    if (!tb->icon || !tb->power) {
        return;
    }

    const IsdeBattery *bat = isde_power_get_battery(tb->power, 0);
    int capacity = bat ? bat->capacity : 0;
    int charging = bat ? (bat->status == ISDE_BAT_CHARGING) : 0;

    (void)state;
    const char *name = icon_name_for_state(capacity, charging);

    char *icon_path = isde_icon_find("status", name);
    if (!icon_path) {
        fprintf(stderr, "isde-panel: tray-battery: cannot find icon %s\n",
                name);
        return;
    }

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgImage(&ab, icon_path);
    IswArgLabel(&ab, "");
    IswSetValues(tb->icon, ab.args, ab.count);

    free(icon_path);
}

void tn_battery_update_icon(TrayBattery *tb)
{
    if (!tb->icon || !tb->power) {
        return;
    }

    int new_state = compute_icon_state(tb);
    if (new_state != tb->icon_state) {
        tb->icon_state = new_state;
        load_icon(tb, new_state);
    }
}

void tn_battery_reload_theme(TrayBattery *tb)
{
    load_icon(tb, tb->icon_state);
}

/* ---------- poll timer ---------- */

static void poll_timer_cb(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayBattery *tb = (TrayBattery *)client_data;

    if (!tb->power) {
        return;
    }

    isde_power_poll(tb->power);
    tn_battery_update_icon(tb);
    tbat_popup_update(tb);

    tb->poll_timer = IswAppAddTimeOut(tb->panel->app, 5000,
                                      poll_timer_cb, tb);
}

/* ---------- click callbacks ---------- */

static void on_icon_click(Widget w, IswPointer client_data,
                          IswPointer call_data)
{
    (void)w; (void)call_data;
    TrayBattery *tb = (TrayBattery *)client_data;
    tbat_popup_show(tb);
}

static void on_icon_right_click(Widget w, IswPointer closure,
                                IswEvent *event, Boolean *cont)
{
    (void)w;
    TrayBattery *tb = (TrayBattery *)closure;

    if (event->kind != IswButtonDown) {
        return;
    }

    if (event->button.button == IswButtonRight) {
        tbat_menu_show(tb);
        *cont = False;
    }
}

/* ---------- deferred initial icon load ---------- */

static void deferred_icon_load(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayBattery *tb = (TrayBattery *)client_data;
    tb->icon_state = compute_icon_state(tb);
    load_icon(tb, tb->icon_state);
}

/* ---------- public API ---------- */

void tn_battery_init(Panel *p)
{
    IsdePower *power = isde_power_init();
    if (!power || isde_power_battery_count(power) == 0) {
        isde_power_free(power);
        return;
    }

    TrayBattery *tb = calloc(1, sizeof(TrayBattery));
    p->tray_battery = tb;
    tb->panel = p;
    tb->power = power;

    tb->icon = panel_tray_add_icon(p, "trayBtn", commandWidgetClass);
    IswAddCallback(tb->icon, IswNcallback, on_icon_click, tb);
    IswAddEventHandler(tb->icon, IswButtonPressMask, False,
                       on_icon_right_click, tb);

    tbat_popup_init(tb);
    tbat_menu_init(tb);

    /* Load small font */
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
    tb->small_font = isde_resolve_font(p->toplevel, spec);
    if (cfg) { isde_config_free(cfg); }

    tb->icon_state = -1;

    /* Start poll timer */
    if (tb->power) {
        tb->poll_timer = IswAppAddTimeOut(p->app, 5000, poll_timer_cb, tb);
    }

    IswAppAddTimeOut(p->app, 100, deferred_icon_load, tb);
}

void tn_battery_cleanup(Panel *p)
{
    TrayBattery *tb = p->tray_battery;
    if (!tb) {
        return;
    }

    tbat_popup_cleanup(tb);
    tbat_menu_cleanup(tb);

    isde_power_free(tb->power);
    tb->power = NULL;

    if (tb->icon) {
        panel_tray_remove_icon(p, tb->icon);
        tb->icon = NULL;
    }

    free(tb);
    p->tray_battery = NULL;
}
