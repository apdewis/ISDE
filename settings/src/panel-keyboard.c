#define _POSIX_C_SOURCE 200809L
/*
 * panel-keyboard.c — Keyboard settings: repeat delay and interval
 */
#include "settings.h"

#include <stdlib.h>
#include <xcb/xcb.h>
#include <xcb/xkb.h>
#include <ISW/IswArgMacros.h>

static Widget scale_repeat_delay;
static Widget scale_repeat_interval;

static int saved_repeat_delay;
static int saved_repeat_interval;

static IsdeDBus        *panel_dbus;
static xcb_connection_t *panel_conn;

#define SLIDER_W 300
#define LABEL_W 250

static void apply_keyboard(int delay, int interval)
{
    if (!panel_conn) { return; }
    xcb_xkb_use_extension(panel_conn, 1, 0);
    xcb_xkb_set_controls(panel_conn,
        XCB_XKB_ID_USE_CORE_KBD,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        delay, interval,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        XCB_XKB_BOOL_CTRL_REPEAT_KEYS,
        0, NULL);
    xcb_flush(panel_conn);
}

static void read_current_keyboard(void)
{
    if (!panel_conn) { return; }
    xcb_xkb_use_extension(panel_conn, 1, 0);
    xcb_xkb_get_controls_reply_t *reply =
        xcb_xkb_get_controls_reply(panel_conn,
            xcb_xkb_get_controls(panel_conn, XCB_XKB_ID_USE_CORE_KBD),
            NULL);
    if (reply) {
        saved_repeat_delay = reply->repeatDelay;
        saved_repeat_interval = reply->repeatInterval;
        free(reply);
    }
}

static void keyboard_apply(void)
{
    int delay    = IswSliderGetValue(scale_repeat_delay);
    int interval = IswSliderGetValue(scale_repeat_interval);

    apply_keyboard(delay, interval);

    char *path = isde_xdg_config_path("isde.toml");
    if (path) {
        isde_config_write_int(path, "keyboard", "repeat_delay", delay);
        isde_config_write_int(path, "keyboard", "repeat_interval", interval);
        free(path);
    }

    saved_repeat_delay = delay;
    saved_repeat_interval = interval;

    if (panel_dbus) {
        isde_dbus_settings_notify(panel_dbus, "keyboard", "*");
    }
}

static void keyboard_revert(void)
{
    IswSliderSetValue(scale_repeat_delay, saved_repeat_delay);
    IswSliderSetValue(scale_repeat_interval, saved_repeat_interval);
    apply_keyboard(saved_repeat_delay, saved_repeat_interval);
}

static Widget make_scale_row(Widget form, Widget above, const char *label_text,
                             int min, int max, int value, Widget *out_scale)
{
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgLabel(&ab, label_text);
    IswArgBorderWidth(&ab, 0);
    IswArgWidth(&ab, LABEL_W);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    if (above) { IswArgFromVert(&ab, above); }
    Widget lbl = IswCreateManagedWidget("lbl", labelWidgetClass,
                                       form, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgFromHoriz(&ab, lbl);
    if (above) { IswArgFromVert(&ab, above); }
    IswArgMinimumValue(&ab, min);
    IswArgMaximumValue(&ab, max);
    IswArgSliderValue(&ab, value);
    IswArgOrientation(&ab, XtorientHorizontal);
    IswArgShowValue(&ab, True);
    IswArgWidth(&ab, SLIDER_W);
    IswArgBorderWidth(&ab, 0);
    IswArgLeft(&ab, IswChainLeft);
    *out_scale = IswCreateManagedWidget("slider", sliderWidgetClass,
                                       form, ab.args, ab.count);
    return *out_scale;
}

static Widget keyboard_create(Widget parent, IswAppContext app)
{
    (void)app;

    panel_conn = IswDisplay(IswParent(parent));

    read_current_keyboard();

    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *kb = isde_config_table(root, "keyboard");
        if (kb) {
            saved_repeat_delay = (int)isde_config_int(kb, "repeat_delay",
                                                       saved_repeat_delay);
            saved_repeat_interval = (int)isde_config_int(kb, "repeat_interval",
                                                          saved_repeat_interval);
        }
        isde_config_free(cfg);
    }

    Dimension pw;
    IswArgBuilder qb = IswArgBuilderInit();
    IswArgWidth(&qb, &pw);
    IswGetValues(parent, qb.args, qb.count);

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgDefaultDistance(&ab, 8);
    IswArgBorderWidth(&ab, 0);
    Widget form = IswCreateWidget("keyboardPanel", formWidgetClass,
                                 parent, ab.args, ab.count);

    Widget row;
    row = make_scale_row(form, NULL, "Key repeat delay (ms):",
                         100, 1000, saved_repeat_delay, &scale_repeat_delay);
    row = make_scale_row(form, row, "Key repeat interval (ms):",
                         10, 200, saved_repeat_interval, &scale_repeat_interval);

    return form;
}

static int keyboard_has_changes(void)
{
    if (!scale_repeat_delay) { return 0; }
    return IswSliderGetValue(scale_repeat_delay) != saved_repeat_delay ||
           IswSliderGetValue(scale_repeat_interval) != saved_repeat_interval;
}

static void keyboard_destroy(void)
{
    scale_repeat_delay = NULL;
    scale_repeat_interval = NULL;
}

void panel_keyboard_set_dbus(IsdeDBus *bus) { panel_dbus = bus; }

const IsdeSettingsPanel panel_keyboard = {
    .name        = "Keyboard",
    .icon        = NULL,
    .section     = "keyboard",
    .create      = keyboard_create,
    .apply       = keyboard_apply,
    .revert      = keyboard_revert,
    .has_changes = keyboard_has_changes,
    .destroy     = keyboard_destroy,
};
