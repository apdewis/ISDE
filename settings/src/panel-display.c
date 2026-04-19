#define _POSIX_C_SOURCE 200809L
/*
 * panel-display.c — Display settings: output selection, HiDPI scaling
 *
 * Uses xcb-randr to enumerate outputs and read/write DPI scaling.
 */
#include "settings.h"
#include <ISW/List.h>
#include <ISW/Slider.h>
#include <ISW/IswArgMacros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/randr.h>

#include "isde/isde-config.h"

typedef struct {
    char    *name;
    uint16_t width;
    uint16_t height;
    int      is_primary;
} OutputInfo;

static OutputInfo *outputs;
static int         noutputs;
static String     *output_names;

static Widget output_list;
static Widget scale_slider;
static Widget scale_label;
static Widget res_label;

static int    selected_output;
static int    saved_scale;
static int    current_scale;

static IsdeDBus *display_dbus;

#define LIST_W 300
#define SLIDER_W 300
#define LABEL_W 150

static void free_outputs(void)
{
    for (int i = 0; i < noutputs; i++) {
        free(outputs[i].name);
    }
    free(outputs);
    free(output_names);
    outputs = NULL;
    output_names = NULL;
    noutputs = 0;
}

static xcb_screen_t *display_screen; /* for fallback */

static void query_outputs(xcb_connection_t *conn, xcb_window_t root)
{
    free_outputs();

    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, root), NULL);

    int cap = 4;
    outputs = malloc(cap * sizeof(OutputInfo));

    if (!res) { goto fallback; }

    xcb_randr_get_output_primary_reply_t *pri =
        xcb_randr_get_output_primary_reply(conn,
            xcb_randr_get_output_primary(conn, root), NULL);
    xcb_randr_output_t primary_id = pri ? pri->output : XCB_NONE;
    free(pri);

    xcb_randr_output_t *outs = xcb_randr_get_screen_resources_current_outputs(res);
    int nouts = xcb_randr_get_screen_resources_current_outputs_length(res);

    for (int i = 0; i < nouts; i++) {
        xcb_randr_get_output_info_reply_t *oinfo =
            xcb_randr_get_output_info_reply(conn,
                xcb_randr_get_output_info(conn, outs[i], XCB_CURRENT_TIME),
                NULL);
        if (!oinfo) { continue; }

        /* Only show connected outputs */
        if (oinfo->connection != XCB_RANDR_CONNECTION_CONNECTED) {
            free(oinfo);
            continue;
        }

        if (noutputs >= cap) {
            cap *= 2;
            outputs = realloc(outputs, cap * sizeof(OutputInfo));
        }

        OutputInfo *o = &outputs[noutputs];
        int namelen = xcb_randr_get_output_info_name_length(oinfo);
        uint8_t *namedata = xcb_randr_get_output_info_name(oinfo);
        o->name = strndup((char *)namedata, namelen);
        o->is_primary = (outs[i] == primary_id);
        o->width = 0;
        o->height = 0;

        if (oinfo->crtc != XCB_NONE) {
            xcb_randr_get_crtc_info_reply_t *cinfo =
                xcb_randr_get_crtc_info_reply(conn,
                    xcb_randr_get_crtc_info(conn, oinfo->crtc,
                                            XCB_CURRENT_TIME), NULL);
            if (cinfo) {
                o->width = cinfo->width;
                o->height = cinfo->height;
                free(cinfo);
            }
        }

        noutputs++;
        free(oinfo);
    }

    if (res) { free(res); }

fallback:
    /* Fallback: if RandR returned nothing, use the X screen */
    if (noutputs == 0 && display_screen) {
        outputs = malloc(sizeof(OutputInfo));
        outputs[0].name = strdup("Screen 0");
        outputs[0].width = display_screen->width_in_pixels;
        outputs[0].height = display_screen->height_in_pixels;
        outputs[0].is_primary = 1;
        noutputs = 1;
    }

    /* Build display name strings */
    output_names = malloc((noutputs + 1) * sizeof(String));
    for (int i = 0; i < noutputs; i++) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s%s (%ux%u)",
                 outputs[i].name,
                 outputs[i].is_primary ? " [Primary]" : "",
                 outputs[i].width, outputs[i].height);
        output_names[i] = strdup(buf);
    }
    output_names[noutputs] = NULL;
}

/* ---------- load/save scaling ---------- */

static int load_scale_from_config(void)
{
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (!cfg) { return 100; }

    IsdeConfigTable *root = isde_config_root(cfg);
    IsdeConfigTable *disp = isde_config_table(root, "display");
    int val = 100;
    if (disp) {
        val = (int)isde_config_int(disp, "scale_percent", 100);
    }
    isde_config_free(cfg);
    return val;
}


static void update_res_label(void)
{
    if (!res_label || selected_output < 0 || selected_output >= noutputs) {
        return;
    }
    OutputInfo *o = &outputs[selected_output];
    char buf[64];
    snprintf(buf, sizeof(buf), "%ux%u", o->width, o->height);
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgLabel(&ab, buf);
    IswSetValues(res_label, ab.args, ab.count);
}

/* ---------- callbacks ---------- */

static void output_select_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)cd;
    IswListReturnStruct *ret = (IswListReturnStruct *)call;
    if (ret->list_index >= 0 && ret->list_index < noutputs) {
        selected_output = ret->list_index;
        update_res_label();
    }
}

static void scale_changed_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)cd;
    int *val = (int *)call;
    current_scale = *val;
}

static void display_apply(void)
{
    char *path = isde_xdg_config_path("isde.toml");
    if (!path) { return; }

    isde_config_write_int(path, "display", "scale_percent", current_scale);
    saved_scale = current_scale;
    free(path);

    if (display_dbus) {
        isde_dbus_settings_notify(display_dbus, "display", "scale_percent");
    }
}

static void display_revert(void)
{
    current_scale = saved_scale;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgSliderValue(&ab, saved_scale);
    IswSetValues(scale_slider, ab.args, ab.count);
}

/* ---------- create ---------- */

static Widget display_create(Widget parent, IswAppContext app)
{
    (void)app;

    Dimension pw, ph;
    IswArgBuilder qb = IswArgBuilderInit();
    IswArgWidth(&qb, &pw);
    IswArgHeight(&qb, &ph);
    IswGetValues(parent, qb.args, qb.count);

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgDefaultDistance(&ab, 8);
    IswArgBorderWidth(&ab, 0);
    Widget form = IswCreateWidget("displayForm", formWidgetClass,
                                 parent, ab.args, ab.count);

    /* Query outputs */
    xcb_connection_t *conn = IswDisplay(parent);
    display_screen = IswScreen(parent);
    query_outputs(conn, display_screen->root);

    saved_scale = load_scale_from_config();
    current_scale = saved_scale;

    Widget prev = NULL;

    /* --- Output list (label-left) --- */
    int lbl_w = 180;
    int ctrl_w = (pw > 0 ? (int)pw - lbl_w - 8 * 4 : 200);
    if (ctrl_w < 100) { ctrl_w = 100; }

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Outputs:");
    IswArgBorderWidth(&ab, 0);
    IswArgWidth(&ab, LABEL_W);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgLeft(&ab, IswChainLeft);
    Widget out_lbl = IswCreateManagedWidget("lbl", labelWidgetClass,
                                           form, ab.args, ab.count);

    int list_height = (ph > 0 ? ph / 3 : 100);
    IswArgBuilderReset(&ab);
    IswArgList(&ab, output_names);
    IswArgNumberStrings(&ab, noutputs);
    IswArgDefaultColumns(&ab, 1);
    IswArgForceColumns(&ab, True);
    IswArgVerticalList(&ab, True);
    IswArgHeight(&ab, LIST_W);
    IswArgBorderWidth(&ab, 0);
    IswArgFromHoriz(&ab, out_lbl);
    IswArgLeft(&ab, IswChainLeft);
    output_list = IswCreateManagedWidget("outputList", listWidgetClass,
                                        form, ab.args, ab.count);
    IswAddCallback(output_list, IswNcallback, output_select_cb, NULL);

    /* Select primary by default */
    selected_output = 0;
    for (int i = 0; i < noutputs; i++) {
        if (outputs[i].is_primary) { selected_output = i; break; }
    }
    IswListHighlight(output_list, selected_output);
    prev = output_list;

    /* --- Resolution (read-only, label-left) --- */

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Resolution:");
    IswArgBorderWidth(&ab, 0);
    IswArgFromVert(&ab, prev);
    IswArgWidth(&ab, LABEL_W);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    Widget res_lbl_static = IswCreateManagedWidget("lbl", labelWidgetClass,
                                                   form, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "");
    IswArgBorderWidth(&ab, 0);
    IswArgFromVert(&ab, prev);
    IswArgFromHoriz(&ab, res_lbl_static);
    IswArgWidth(&ab, LABEL_W);
    IswArgJustify(&ab, IswJustifyLeft);
    IswArgLeft(&ab, IswChainLeft);
    res_label = IswCreateManagedWidget("resValue", labelWidgetClass,
                                      form, ab.args, ab.count);
    update_res_label();
    prev = res_label;

    /* --- HiDPI scale (label-left) --- */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Scale:");
    IswArgBorderWidth(&ab, 0);
    IswArgFromVert(&ab, prev);
    IswArgWidth(&ab, LABEL_W);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    scale_label = IswCreateManagedWidget("lbl", labelWidgetClass,
                                        form, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgMinimumValue(&ab, 100);
    IswArgMaximumValue(&ab, 300);
    IswArgSliderValue(&ab, current_scale);
    IswArgShowValue(&ab, True);
    IswArgTickInterval(&ab, 25);
    IswArgBorderWidth(&ab, 0);
    IswArgFromVert(&ab, prev);
    IswArgFromHoriz(&ab, scale_label);
    IswArgWidth(&ab, SLIDER_W);
    IswArgLeft(&ab, IswChainLeft);
    scale_slider = IswCreateManagedWidget("scaleSlider", sliderWidgetClass,
                                          form, ab.args, ab.count);
    IswAddCallback(scale_slider, IswNvalueChanged, scale_changed_cb, NULL);
    prev = scale_slider;

    return form;
}

static int display_has_changes(void)
{
    return current_scale != saved_scale;
}

static void display_destroy(void)
{
    output_list = NULL;
    scale_slider = NULL;
    scale_label = NULL;
    res_label = NULL;
}

void panel_display_set_dbus(IsdeDBus *bus) { display_dbus = bus; }

const IsdeSettingsPanel panel_display = {
    .name        = "Display",
    .icon        = NULL,
    .section     = "display",
    .create      = display_create,
    .apply       = display_apply,
    .revert      = display_revert,
    .has_changes = display_has_changes,
    .destroy     = display_destroy,
};
