#define _POSIX_C_SOURCE 200809L
/*
 * panel-input.c — Mouse settings: double-click speed, acceleration, threshold
 */
#include "settings.h"

#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

static Widget scale_dclick;
static Widget scale_accel;
static Widget scale_threshold;

static int saved_dclick;
static int saved_accel_num;
static int saved_threshold;

static IsdeDBus        *panel_dbus;
static xcb_connection_t *panel_conn;

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

static int lbl_w;
static int scale_w;

static Widget make_scale_row(Widget form, Widget above, const char *label_text,
                             int min, int max, int value, Widget *out_scale)
{
    Arg args[20];
    Cardinal n;

    n = 0;
    XtSetArg(args[n], XtNlabel, label_text);              n++;
    XtSetArg(args[n], XtNborderWidth, 0);                  n++;
    XtSetArg(args[n], XtNwidth, lbl_w);                    n++;
    XtSetArg(args[n], XtNjustify, XtJustifyRight);         n++;
    XtSetArg(args[n], XtNleft, XtChainLeft);               n++;
    XtSetArg(args[n], XtNright, XtChainLeft);              n++;
    if (above) { XtSetArg(args[n], XtNfromVert, above); n++; }
    Widget lbl = XtCreateManagedWidget("lbl", labelWidgetClass,
                                       form, args, n);

    n = 0;
    XtSetArg(args[n], XtNfromHoriz, lbl);                   n++;
    if (above) { XtSetArg(args[n], XtNfromVert, above); n++; }
    XtSetArg(args[n], XtNminimumValue, min);                 n++;
    XtSetArg(args[n], XtNmaximumValue, max);                 n++;
    XtSetArg(args[n], XtNsliderValue, value);                 n++;
    XtSetArg(args[n], XtNorientation, XtorientHorizontal);   n++;
    XtSetArg(args[n], XtNshowValue, True);                   n++;
    XtSetArg(args[n], XtNwidth, scale_w);                    n++;
    XtSetArg(args[n], XtNborderWidth, 0);                    n++;
    XtSetArg(args[n], XtNresizable, True);                   n++;
    XtSetArg(args[n], XtNleft, XtChainLeft);                 n++;
    XtSetArg(args[n], XtNright, XtChainRight);               n++;
    *out_scale = XtCreateManagedWidget("slider", sliderWidgetClass,
                                       form, args, n);
    return *out_scale;
}

static Widget input_create(Widget parent, XtAppContext app)
{
    (void)app;

    panel_conn = XtDisplay(XtParent(parent));

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

    Dimension pw;
    Arg qa[20];
    XtSetArg(qa[0], XtNwidth, &pw);
    XtGetValues(parent, qa, 1);

    lbl_w = 180;
    scale_w = (pw > 0 ? (int)pw - lbl_w - 8 * 4 : 200);
    if (scale_w < 100) { scale_w = 100; }

    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNdefaultDistance, 8); n++;
    XtSetArg(args[n], XtNborderWidth, 0);    n++;
    Widget form = XtCreateWidget("mousePanel", formWidgetClass,
                                 parent, args, n);

    Widget row;
    row = make_scale_row(form, NULL, "Double-click speed (ms):",
                         100, 1000, saved_dclick, &scale_dclick);
    row = make_scale_row(form, row, "Mouse acceleration:",
                         1, 10, saved_accel_num, &scale_accel);
    row = make_scale_row(form, row, "Mouse threshold (pixels):",
                         1, 20, saved_threshold, &scale_threshold);

    return form;
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
    .icon        = NULL,
    .section     = "input",
    .create      = input_create,
    .apply       = input_apply,
    .revert      = input_revert,
    .has_changes = input_has_changes,
    .destroy     = input_destroy,
};
