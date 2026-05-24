#define _POSIX_C_SOURCE 200809L
/*
 * panel-input.c — Mouse settings: double-click speed, acceleration, threshold
 */
#include "settings.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xinput.h>
#include <ISW/IswArgMacros.h>

static Widget scale_dclick;
static Widget scale_accel;
static Widget scale_threshold;

static int saved_dclick;
static int saved_accel_num;
static int saved_threshold;

static IsdeDBus        *panel_dbus;
static xcb_connection_t *panel_conn;

static xcb_atom_t atom_libinput_accel_speed;
static xcb_atom_t atom_float;

#define SLIDER_W 300
#define LABEL_W 250

static xcb_atom_t intern_atom(xcb_connection_t *c, const char *name)
{
    xcb_intern_atom_cookie_t ck = xcb_intern_atom(c, 1, strlen(name), name);
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, ck, NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

static float slider_to_libinput_speed(int value)
{
    return (value - 1) / 9.0f * 2.0f - 1.0f;
}

static int libinput_speed_to_slider(float speed)
{
    int v = (int)roundf((speed + 1.0f) / 2.0f * 9.0f) + 1;
    if (v < 1) { v = 1; }
    if (v > 10) { v = 10; }
    return v;
}

static int set_libinput_accel_speed(xcb_input_device_id_t devid, float speed)
{
    if (atom_libinput_accel_speed == XCB_ATOM_NONE ||
        atom_float == XCB_ATOM_NONE) {
        return 0;
    }

    uint32_t val;
    memcpy(&val, &speed, sizeof(val));

    xcb_input_xi_change_property_items_t items;
    items.data32 = &val;

    xcb_input_xi_change_property_aux(panel_conn, devid,
        XCB_PROP_MODE_REPLACE,
        XCB_INPUT_PROPERTY_FORMAT_32_BITS,
        atom_libinput_accel_speed, atom_float,
        1, &items);
    return 1;
}

static int get_libinput_accel_speed(xcb_input_device_id_t devid, float *out)
{
    if (atom_libinput_accel_speed == XCB_ATOM_NONE ||
        atom_float == XCB_ATOM_NONE) {
        return 0;
    }

    xcb_input_xi_get_property_reply_t *r =
        xcb_input_xi_get_property_reply(panel_conn,
            xcb_input_xi_get_property(panel_conn, devid, 0,
                atom_libinput_accel_speed, atom_float, 0, 1),
            NULL);
    if (!r) { return 0; }
    if (r->type != atom_float || r->num_items < 1 || r->format != 32) {
        free(r);
        return 0;
    }

    uint32_t *data = xcb_input_xi_get_property_items(r);
    memcpy(out, data, sizeof(float));
    free(r);
    return 1;
}

typedef void (*device_cb)(xcb_input_device_id_t devid, void *ctx);

static void for_each_slave_pointer(device_cb cb, void *ctx)
{
    xcb_input_xi_query_device_reply_t *r =
        xcb_input_xi_query_device_reply(panel_conn,
            xcb_input_xi_query_device(panel_conn, XCB_INPUT_DEVICE_ALL),
            NULL);
    if (!r) { return; }

    xcb_input_xi_device_info_iterator_t it =
        xcb_input_xi_query_device_infos_iterator(r);
    for (; it.rem; xcb_input_xi_device_info_next(&it)) {
        if (it.data->type == XCB_INPUT_DEVICE_TYPE_SLAVE_POINTER &&
            it.data->enabled) {
            cb(it.data->deviceid, ctx);
        }
    }
    free(r);
}

struct apply_ctx {
    float speed;
    int any_libinput;
};

static void apply_cb(xcb_input_device_id_t devid, void *ctx)
{
    struct apply_ctx *ac = ctx;
    if (set_libinput_accel_speed(devid, ac->speed)) {
        ac->any_libinput = 1;
    }
}

static void apply_mouse(int accel_num, int threshold)
{
    if (!panel_conn) { return; }

    struct apply_ctx ac = {
        .speed = slider_to_libinput_speed(accel_num),
        .any_libinput = 0,
    };
    for_each_slave_pointer(apply_cb, &ac);

    if (!ac.any_libinput) {
        xcb_change_pointer_control(panel_conn,
                                   accel_num, 1, threshold, 1, 1);
    }
    xcb_flush(panel_conn);
}

struct read_ctx {
    int found;
    float speed;
};

static void read_cb(xcb_input_device_id_t devid, void *ctx)
{
    struct read_ctx *rc = ctx;
    if (rc->found) { return; }
    if (get_libinput_accel_speed(devid, &rc->speed)) {
        rc->found = 1;
    }
}

static void read_current_mouse(void)
{
    if (!panel_conn) { return; }

    struct read_ctx rc = { .found = 0, .speed = 0.0f };
    for_each_slave_pointer(read_cb, &rc);

    if (rc.found) {
        saved_accel_num = libinput_speed_to_slider(rc.speed);
        saved_threshold = 0;
        return;
    }

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
    IswArgOrientation(&ab, IswOrientHorizontal);
    IswArgShowValue(&ab, True);
    IswArgWidth(&ab, SLIDER_W);
    IswArgBorderWidth(&ab, 0);
    IswArgLeft(&ab, IswChainLeft);
    *out_scale = IswCreateManagedWidget("slider", sliderWidgetClass,
                                       form, ab.args, ab.count);
    return *out_scale;
}

static Widget input_create(Widget parent, IswAppContext app)
{
    (void)app;

    panel_conn = IswDisplay(IswParent(parent));

    atom_libinput_accel_speed = intern_atom(panel_conn,
                                            "libinput Accel Speed");
    atom_float = intern_atom(panel_conn, "FLOAT");

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
    IswArgBuilder qb = IswArgBuilderInit();
    IswArgWidth(&qb, &pw);
    IswGetValues(parent, qb.args, qb.count);

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgDefaultDistance(&ab, 8);
    IswArgBorderWidth(&ab, 0);
    Widget form = IswCreateWidget("mousePanel", formWidgetClass,
                                 parent, ab.args, ab.count);

    Widget row;
    row = make_scale_row(form, NULL, "Double-click speed (ms):",
                         100, 1000, saved_dclick, &scale_dclick);
    row = make_scale_row(form, row, "Pointer speed:",
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
