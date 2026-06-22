#define _POSIX_C_SOURCE 200809L
/*
 * panel-input.c — Mouse settings: double-click speed, acceleration, threshold
 */
#include "settings.h"

#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <ISW/IswArgMacros.h>
#include <ISW/ISWPlatform.h>

static Widget scale_dclick;
static Widget scale_accel;
static Widget scale_threshold;

static int saved_dclick;
static int saved_accel_num;
static int saved_threshold;

static IsdeDBus        *panel_dbus;
static xcb_connection_t *panel_conn;

#define SLIDER_W 300
#define LABEL_W 250

static void apply_mouse(int accel_num, int threshold)
{
    if (!panel_conn) { return; }
    xcb_change_pointer_control(panel_conn,
                               accel_num, 1, threshold, 1, 1);
    xcb_flush(panel_conn);
}

static void read_current_mouse(void)
{
    if (!panel_conn) { return; }
    xcb_get_pointer_control_reply_t *reply =
        xcb_get_pointer_control_reply(panel_conn,
            xcb_get_pointer_control(panel_conn), NULL);
    if (reply) {
        saved_accel_num = reply->acceleration_numerator;
        saved_threshold = reply->threshold;
        free(reply);
    }
}

static void input_apply(void)
{
    int dclick = IswSliderGetValue(scale_dclick);
    int accel  = IswSliderGetValue(scale_accel);
    int thresh = IswSliderGetValue(scale_threshold);

    apply_mouse(accel, thresh);

    char *path = isde_xdg_config_path("isde.toml");
    if (path) {
        isde_config_write_int(path, "input", "double_click_ms", dclick);
        isde_config_write_int(path, "input", "mouse_acceleration", accel);
        isde_config_write_int(path, "input", "mouse_threshold", thresh);
        free(path);
    }

    saved_dclick = dclick;
    saved_accel_num = accel;
    saved_threshold = thresh;

    isde_config_invalidate_cache();
    if (panel_dbus) {
        isde_dbus_settings_notify(panel_dbus, "input", "*");
    }
}

static void input_revert(void)
{
    IswSliderSetValue(scale_dclick, saved_dclick);
    IswSliderSetValue(scale_accel, saved_accel_num);
    IswSliderSetValue(scale_threshold, saved_threshold);
    apply_mouse(saved_accel_num, saved_threshold);
}

static void make_scale_row(Widget vbox, const char *label_text,
                           int min, int max, int value, Widget *out_scale)
{
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgOrientation(&ab, IswOrientHorizontal);
    IswArgSpacing(&ab, 8);
    Widget row = IswCreateManagedWidget("row", flexBoxWidgetClass,
                                       vbox, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, label_text);
    IswArgBorderWidth(&ab, 0);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgFlexBasis(&ab, LABEL_W);
    IswArgFlexAlign(&ab, IswFlexAlignCenter);
    IswCreateManagedWidget("lbl", labelWidgetClass, row, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgMinimumValue(&ab, min);
    IswArgMaximumValue(&ab, max);
    IswArgSliderValue(&ab, value);
    IswArgOrientation(&ab, IswOrientHorizontal);
    IswArgShowValue(&ab, True);
    IswArgWidth(&ab, SLIDER_W);
    IswArgBorderWidth(&ab, 0);
    IswArgFlexAlign(&ab, IswFlexAlignCenter);
    *out_scale = IswCreateManagedWidget("slider", sliderWidgetClass,
                                       row, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "");
    IswArgBorderWidth(&ab, 0);
    IswArgFlexBasis(&ab, 20);
    IswCreateManagedWidget("spacer", labelWidgetClass, row, ab.args, ab.count);
}

static Widget input_create(Widget parent, IswAppContext app)
{
    (void)app;

    panel_conn = (xcb_connection_t *)IswDisplayNativeHandle(
        IswDisplayOf(IswParent(parent)));

    saved_dclick = isde_config_double_click_ms();
    read_current_mouse();

    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *input = isde_config_table(root, "input");
        if (input) {
            saved_accel_num = (int)isde_config_int(input, "mouse_acceleration",
                                                    saved_accel_num);
            saved_threshold = (int)isde_config_int(input, "mouse_threshold",
                                                    saved_threshold);
        }
        isde_config_free(cfg);
    }

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgOrientation(&ab, IswOrientVertical);
    IswArgSpacing(&ab, 8);
    IswArgBorderWidth(&ab, 0);
    Widget vbox = IswCreateWidget("mousePanel", flexBoxWidgetClass,
                                 parent, ab.args, ab.count);

    make_scale_row(vbox, "Double-click speed (ms):",
                   100, 1000, saved_dclick, &scale_dclick);
    make_scale_row(vbox, "Mouse acceleration:",
                   1, 10, saved_accel_num, &scale_accel);
    make_scale_row(vbox, "Mouse threshold (pixels):",
                   1, 20, saved_threshold, &scale_threshold);

    return vbox;
}

static int input_has_changes(void)
{
    if (!scale_dclick) { return 0; }
    return IswSliderGetValue(scale_dclick) != saved_dclick ||
           IswSliderGetValue(scale_accel) != saved_accel_num ||
           IswSliderGetValue(scale_threshold) != saved_threshold;
}

static void input_destroy(void)
{
    scale_dclick = NULL;
    scale_accel = NULL;
    scale_threshold = NULL;
}

void panel_input_set_dbus(IsdeDBus *bus) { panel_dbus = bus; }

const IsdeSettingsPanel panel_input = {
    .name        = "Mouse",
    .icon        = "input-mouse",
    .section     = "input",
    .create      = input_create,
    .apply       = input_apply,
    .revert      = input_revert,
    .has_changes = input_has_changes,
    .destroy     = input_destroy,
};
