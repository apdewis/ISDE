#define _POSIX_C_SOURCE 200809L
/*
 * panel-display.c — Display settings: output selection, HiDPI scaling
 *
 * Uses xcb-randr to enumerate outputs and read/write DPI scaling.
 */
#include "settings.h"
#include <ISW/List.h>
#include <ISW/Scale.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/randr.h>

#include "isde/isde-config.h"

/* ---------- state ---------- */

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

/* ---------- enumerate outputs ---------- */

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

static void update_scale_label(int percent)
{
    if (!scale_label) { return; }
    char buf[32];
    snprintf(buf, sizeof(buf), "Scale: %d%%", percent);
    Arg a[1];
    XtSetArg(a[0], XtNlabel, buf);
    XtSetValues(scale_label, a, 1);
}

static void update_res_label(void)
{
    if (!res_label || selected_output < 0 || selected_output >= noutputs) {
        return;
    }
    OutputInfo *o = &outputs[selected_output];
    char buf[64];
    snprintf(buf, sizeof(buf), "Resolution: %ux%u", o->width, o->height);
    Arg a[1];
    XtSetArg(a[0], XtNlabel, buf);
    XtSetValues(res_label, a, 1);
}

/* ---------- callbacks ---------- */

static void output_select_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)cd;
    IswListReturnStruct *ret = (IswListReturnStruct *)call;
    if (ret->list_index >= 0 && ret->list_index < noutputs) {
        selected_output = ret->list_index;
        update_res_label();
    }
}

static void scale_changed_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)cd;
    int *val = (int *)call;
    current_scale = *val;
    update_scale_label(current_scale);
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
    Arg a[1];
    XtSetArg(a[0], XtNscaleValue, saved_scale);
    XtSetValues(scale_slider, a, 1);
    update_scale_label(saved_scale);
}

/* ---------- create ---------- */

static Widget display_create(Widget parent, XtAppContext app)
{
    (void)app;

    Arg args[20];
    Cardinal n;
    Dimension pw, ph;
    Arg qargs[20];
    XtSetArg(qargs[0], XtNwidth, &pw);
    XtSetArg(qargs[1], XtNheight, &ph);
    XtGetValues(parent, qargs, 2);

    n = 0;
    XtSetArg(args[n], XtNdefaultDistance, 4); n++;
    XtSetArg(args[n], XtNborderWidth, 0);    n++;
    Widget form = XtCreateWidget("displayForm", formWidgetClass,
                                 parent, args, n);

    /* Query outputs */
    xcb_connection_t *conn = XtDisplay(parent);
    display_screen = XtScreen(parent);
    query_outputs(conn, display_screen->root);

    saved_scale = load_scale_from_config();
    current_scale = saved_scale;

    Widget prev = NULL;

    /* --- Output list --- */
    n = 0;
    XtSetArg(args[n], XtNlabel, "Outputs:");  n++;
    XtSetArg(args[n], XtNborderWidth, 0);      n++;
    Widget out_lbl = XtCreateManagedWidget("outLbl", labelWidgetClass,
                                           form, args, n);
    prev = out_lbl;

    int list_height = (ph > 0 ? ph / 3 : 100);
    n = 0;
    XtSetArg(args[n], XtNlist, output_names);            n++;
    XtSetArg(args[n], XtNnumberStrings, noutputs);       n++;
    XtSetArg(args[n], XtNdefaultColumns, 1);              n++;
    XtSetArg(args[n], XtNforceColumns, True);             n++;
    XtSetArg(args[n], XtNverticalList, True);             n++;
    XtSetArg(args[n], XtNheight, list_height);            n++;
    XtSetArg(args[n], XtNborderWidth, 0);                 n++;
    XtSetArg(args[n], XtNfromVert, prev);                 n++;
    output_list = XtCreateManagedWidget("outputList", listWidgetClass,
                                        form, args, n);
    XtAddCallback(output_list, XtNcallback, output_select_cb, NULL);

    /* Select primary by default */
    selected_output = 0;
    for (int i = 0; i < noutputs; i++) {
        if (outputs[i].is_primary) { selected_output = i; break; }
    }
    IswListHighlight(output_list, selected_output);
    prev = output_list;

    /* --- Resolution label --- */
    n = 0;
    XtSetArg(args[n], XtNlabel, "Resolution:");  n++;
    XtSetArg(args[n], XtNborderWidth, 0);          n++;
    XtSetArg(args[n], XtNfromVert, prev);           n++;
    res_label = XtCreateManagedWidget("resLabel", labelWidgetClass,
                                      form, args, n);
    update_res_label();
    prev = res_label;

    /* --- HiDPI scale --- */
    n = 0;
    XtSetArg(args[n], XtNlabel, "Scale: 100%");  n++;
    XtSetArg(args[n], XtNborderWidth, 0);          n++;
    XtSetArg(args[n], XtNfromVert, prev);           n++;
    scale_label = XtCreateManagedWidget("scaleLbl", labelWidgetClass,
                                        form, args, n);
    update_scale_label(current_scale);
    prev = scale_label;

    n = 0;
    XtSetArg(args[n], XtNminimumValue, 100);       n++;
    XtSetArg(args[n], XtNmaximumValue, 300);        n++;
    XtSetArg(args[n], XtNscaleValue, current_scale); n++;
    XtSetArg(args[n], XtNshowValue, True);           n++;
    XtSetArg(args[n], XtNtickInterval, 25);          n++;
    XtSetArg(args[n], XtNborderWidth, 0);             n++;
    XtSetArg(args[n], XtNfromVert, prev);              n++;
    if (pw > 100) { XtSetArg(args[n], XtNwidth, pw - 40); n++; }
    scale_slider = XtCreateManagedWidget("scaleSlider", scaleWidgetClass,
                                          form, args, n);
    XtAddCallback(scale_slider, XtNvalueChanged, scale_changed_cb, NULL);
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
