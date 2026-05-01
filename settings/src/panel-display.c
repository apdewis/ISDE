#define _POSIX_C_SOURCE 200809L
/*
 * panel-display.c — Display settings: output selection, resolution, HiDPI scaling
 */
#include "settings.h"
#include <ISW/List.h>
#include <ISW/ComboBox.h>
#include <ISW/Slider.h>
#include <ISW/IswArgMacros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/randr.h>

#include "isde/isde-config.h"

typedef struct {
    xcb_randr_mode_t id;
    uint16_t         width;
    uint16_t         height;
    double           refresh;
    int              preferred;
} ModeEntry;

typedef struct {
    char                *name;
    char                *edid_hash;
    xcb_randr_output_t   output_id;
    xcb_randr_crtc_t     crtc_id;
    uint16_t             cur_width;
    uint16_t             cur_height;
    double               cur_refresh;
    xcb_randr_mode_t     cur_mode;
    int16_t              x, y;
    int                  is_primary;

    ModeEntry           *modes;
    int                  nmodes;

    int                  sel_mode_idx;
    int                  sel_scale;

    int                  saved_mode_idx;
    int                  saved_scale;
} OutputInfo;

static OutputInfo *outputs;
static int         noutputs;
static String     *output_names;

static String     *mode_strings;
static int         nmode_strings;

static Widget output_list;
static Widget res_combo;
static Widget scale_slider;
static Widget scale_label;

static int    selected_output;

static IsdeDBus *display_dbus;
static xcb_connection_t *display_conn;
static xcb_window_t      display_root;

#define LABEL_W 150
#define LIST_W  300
#define SLIDER_W 300

/* ---------- helpers ---------- */

static void free_mode_strings(void)
{
    if (mode_strings) {
        for (int i = 0; i < nmode_strings; i++)
            free(mode_strings[i]);
        free(mode_strings);
        mode_strings = NULL;
        nmode_strings = 0;
    }
}

static void free_outputs(void)
{
    for (int i = 0; i < noutputs; i++) {
        free(outputs[i].name);
        free(outputs[i].edid_hash);
        free(outputs[i].modes);
    }
    free(outputs);
    free(output_names);
    outputs = NULL;
    output_names = NULL;
    noutputs = 0;
    free_mode_strings();
}

static double compute_refresh(xcb_randr_mode_info_t *mi)
{
    if (mi->htotal == 0 || mi->vtotal == 0)
        return 0.0;
    double vt = mi->vtotal;
    if (mi->mode_flags & XCB_RANDR_MODE_FLAG_DOUBLE_SCAN)
        vt *= 2;
    if (mi->mode_flags & XCB_RANDR_MODE_FLAG_INTERLACE)
        vt /= 2;
    return (double)mi->dot_clock / ((double)mi->htotal * vt);
}

static char *read_edid_hash(xcb_connection_t *conn, xcb_randr_output_t output)
{
    xcb_intern_atom_reply_t *atom_reply =
        xcb_intern_atom_reply(conn,
            xcb_intern_atom(conn, 1, 4, "EDID"), NULL);
    if (!atom_reply) return NULL;
    xcb_atom_t edid_atom = atom_reply->atom;
    free(atom_reply);
    if (edid_atom == XCB_ATOM_NONE) return NULL;

    xcb_randr_get_output_property_reply_t *prop =
        xcb_randr_get_output_property_reply(conn,
            xcb_randr_get_output_property(conn, output, edid_atom,
                XCB_ATOM_ANY, 0, 128, 0, 0), NULL);
    if (!prop || prop->num_items < 16) {
        free(prop);
        return NULL;
    }

    uint8_t *data = xcb_randr_get_output_property_data(prop);
    char *hex = malloc(33);
    for (int i = 0; i < 16; i++)
        sprintf(hex + i * 2, "%02x", data[i]);
    hex[32] = '\0';
    free(prop);
    return hex;
}

static int mode_cmp(const void *a, const void *b)
{
    const ModeEntry *ma = a, *mb = b;
    if (ma->width != mb->width) return (int)mb->width - (int)ma->width;
    if (ma->height != mb->height) return (int)mb->height - (int)ma->height;
    if (ma->refresh > mb->refresh) return -1;
    if (ma->refresh < mb->refresh) return 1;
    return 0;
}

static xcb_screen_t *display_screen;

static void query_outputs(xcb_connection_t *conn, xcb_window_t root)
{
    free_outputs();

    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, root), NULL);

    int cap = 4;
    outputs = malloc(cap * sizeof(OutputInfo));

    if (!res) goto fallback;

    xcb_randr_mode_info_t *mode_infos =
        xcb_randr_get_screen_resources_current_modes(res);
    int nmi = xcb_randr_get_screen_resources_current_modes_length(res);

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
                xcb_randr_get_output_info(conn, outs[i], res->config_timestamp),
                NULL);
        if (!oinfo) continue;
        if (oinfo->connection != XCB_RANDR_CONNECTION_CONNECTED) {
            free(oinfo);
            continue;
        }

        if (noutputs >= cap) {
            cap *= 2;
            outputs = realloc(outputs, cap * sizeof(OutputInfo));
        }

        OutputInfo *o = &outputs[noutputs];
        memset(o, 0, sizeof(*o));

        int namelen = xcb_randr_get_output_info_name_length(oinfo);
        uint8_t *namedata = xcb_randr_get_output_info_name(oinfo);
        o->name = strndup((char *)namedata, namelen);
        o->output_id = outs[i];
        o->crtc_id = oinfo->crtc;
        o->is_primary = (outs[i] == primary_id);
        o->edid_hash = read_edid_hash(conn, outs[i]);

        /* Build mode list for this output */
        xcb_randr_mode_t *out_modes = xcb_randr_get_output_info_modes(oinfo);
        int out_nmodes = xcb_randr_get_output_info_modes_length(oinfo);
        int num_preferred = oinfo->num_preferred;

        o->modes = malloc(out_nmodes * sizeof(ModeEntry));
        o->nmodes = 0;

        for (int m = 0; m < out_nmodes; m++) {
            xcb_randr_mode_info_t *mi = NULL;
            for (int k = 0; k < nmi; k++) {
                if (mode_infos[k].id == out_modes[m]) {
                    mi = &mode_infos[k];
                    break;
                }
            }
            if (!mi) continue;

            ModeEntry *me = &o->modes[o->nmodes++];
            me->id = mi->id;
            me->width = mi->width;
            me->height = mi->height;
            me->refresh = compute_refresh(mi);
            me->preferred = (m < num_preferred);
        }

        qsort(o->modes, o->nmodes, sizeof(ModeEntry), mode_cmp);

        /* De-duplicate: keep highest refresh per resolution */
        int dst = 0;
        for (int m = 0; m < o->nmodes; m++) {
            if (dst > 0 &&
                o->modes[m].width == o->modes[dst - 1].width &&
                o->modes[m].height == o->modes[dst - 1].height)
                continue;
            o->modes[dst++] = o->modes[m];
        }
        o->nmodes = dst;

        /* Current CRTC info */
        o->cur_width = 0;
        o->cur_height = 0;
        o->cur_mode = XCB_NONE;
        if (oinfo->crtc != XCB_NONE) {
            xcb_randr_get_crtc_info_reply_t *cinfo =
                xcb_randr_get_crtc_info_reply(conn,
                    xcb_randr_get_crtc_info(conn, oinfo->crtc,
                                            res->config_timestamp), NULL);
            if (cinfo) {
                o->cur_width = cinfo->width;
                o->cur_height = cinfo->height;
                o->cur_mode = cinfo->mode;
                o->x = cinfo->x;
                o->y = cinfo->y;
                free(cinfo);
            }
        }

        /* Find current refresh rate from mode info */
        for (int k = 0; k < nmi; k++) {
            if (mode_infos[k].id == o->cur_mode) {
                o->cur_refresh = compute_refresh(&mode_infos[k]);
                break;
            }
        }

        /* Find sel_mode_idx matching current mode */
        o->sel_mode_idx = 0;
        for (int m = 0; m < o->nmodes; m++) {
            if (o->modes[m].id == o->cur_mode) {
                o->sel_mode_idx = m;
                break;
            }
        }

        noutputs++;
        free(oinfo);
    }

    free(res);

fallback:
    if (noutputs == 0 && display_screen) {
        outputs = malloc(sizeof(OutputInfo));
        memset(&outputs[0], 0, sizeof(OutputInfo));
        outputs[0].name = strdup("Screen 0");
        outputs[0].cur_width = display_screen->width_in_pixels;
        outputs[0].cur_height = display_screen->height_in_pixels;
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
                 outputs[i].cur_width, outputs[i].cur_height);
        output_names[i] = strdup(buf);
    }
    output_names[noutputs] = NULL;
}

/* ---------- load/save per-output config ---------- */

static void load_output_configs(void)
{
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (!cfg) return;

    IsdeConfigTable *root = isde_config_root(cfg);
    IsdeConfigTable *disp = isde_config_table(root, "display");
    int global_scale = 100;
    if (disp)
        global_scale = (int)isde_config_int(disp, "scale_percent", 100);

    IsdeConfigTable *outs_tbl = disp ? isde_config_table(disp, "outputs") : NULL;

    for (int i = 0; i < noutputs; i++) {
        OutputInfo *o = &outputs[i];
        o->sel_scale = global_scale;
        o->saved_scale = global_scale;

        if (!outs_tbl) continue;

        IsdeConfigTable *mon = isde_config_table(outs_tbl, o->name);
        if (!mon) continue;

        int scale = (int)isde_config_int(mon, "scale_percent", global_scale);
        o->sel_scale = scale;
        o->saved_scale = scale;

        int w = (int)isde_config_int(mon, "width", 0);
        int h = (int)isde_config_int(mon, "height", 0);
        if (w > 0 && h > 0) {
            for (int m = 0; m < o->nmodes; m++) {
                if (o->modes[m].width == w && o->modes[m].height == h) {
                    o->sel_mode_idx = m;
                    o->saved_mode_idx = m;
                    break;
                }
            }
        }
    }

    isde_config_free(cfg);
}

static void save_output_configs(void)
{
    char *path = isde_xdg_config_path("isde.toml");
    if (!path) return;

    for (int i = 0; i < noutputs; i++) {
        OutputInfo *o = &outputs[i];
        ModeEntry *m = &o->modes[o->sel_mode_idx];
        char section[128];
        snprintf(section, sizeof(section), "display.outputs.%s", o->name);
        isde_config_write_int(path, section, "width", m->width);
        isde_config_write_int(path, section, "height", m->height);
        isde_config_write_double(path, section, "refresh", m->refresh);
        isde_config_write_int(path, section, "scale_percent", o->sel_scale);
        if (o->edid_hash)
            isde_config_write_string(path, section, "edid", o->edid_hash);
    }

    /* Global scale = primary output's scale */
    for (int i = 0; i < noutputs; i++) {
        if (outputs[i].is_primary) {
            isde_config_write_int(path, "display", "scale_percent",
                                  outputs[i].sel_scale);
            break;
        }
    }

    free(path);
}

/* ---------- mode combo management ---------- */

static void build_mode_strings(OutputInfo *o)
{
    free_mode_strings();
    if (!o || o->nmodes == 0) return;

    nmode_strings = o->nmodes;
    mode_strings = malloc(nmode_strings * sizeof(String));
    for (int i = 0; i < o->nmodes; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%ux%u @ %.0f Hz%s",
                 o->modes[i].width, o->modes[i].height,
                 o->modes[i].refresh,
                 o->modes[i].preferred ? " (preferred)" : "");
        mode_strings[i] = strdup(buf);
    }
}

static void update_mode_combo(void)
{
    if (!res_combo || selected_output < 0 || selected_output >= noutputs)
        return;
    OutputInfo *o = &outputs[selected_output];
    build_mode_strings(o);
    IswListChange(res_combo, mode_strings, nmode_strings, 0, True);
    if (o->sel_mode_idx >= 0 && o->sel_mode_idx < o->nmodes)
        IswListHighlight(res_combo, o->sel_mode_idx);
}

static void update_scale_slider(void)
{
    if (!scale_slider || selected_output < 0 || selected_output >= noutputs)
        return;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgSliderValue(&ab, outputs[selected_output].sel_scale);
    IswSetValues(scale_slider, ab.args, ab.count);
}

/* ---------- callbacks ---------- */

static void output_select_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)cd;
    IswListReturnStruct *ret = (IswListReturnStruct *)call;
    if (ret->list_index >= 0 && ret->list_index < noutputs) {
        selected_output = ret->list_index;
        update_mode_combo();
        update_scale_slider();
    }
}

static void mode_select_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)cd;
    IswListReturnStruct *ret = (IswListReturnStruct *)call;
    if (selected_output >= 0 && selected_output < noutputs &&
        ret->list_index >= 0 &&
        ret->list_index < outputs[selected_output].nmodes) {
        outputs[selected_output].sel_mode_idx = ret->list_index;
    }
}

static void scale_changed_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)cd;
    int *val = (int *)call;
    if (selected_output >= 0 && selected_output < noutputs)
        outputs[selected_output].sel_scale = *val;
}

/* ---------- apply via xcb-randr ---------- */

static void display_apply(void)
{
    if (!display_conn) return;

    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(display_conn,
            xcb_randr_get_screen_resources_current(display_conn, display_root),
            NULL);
    if (!res) return;

    xcb_timestamp_t cfg_ts = res->config_timestamp;

    /* Compute new bounding box */
    int max_x = 0, max_y = 0;
    for (int i = 0; i < noutputs; i++) {
        OutputInfo *o = &outputs[i];
        if (o->crtc_id == XCB_NONE || o->nmodes == 0) continue;
        ModeEntry *m = &o->modes[o->sel_mode_idx];
        int right = o->x + m->width;
        int bottom = o->y + m->height;
        if (right > max_x) max_x = right;
        if (bottom > max_y) max_y = bottom;
    }

    /* Current screen size */
    int scr_w = display_screen->width_in_pixels;
    int scr_h = display_screen->height_in_pixels;
    int scr_mm_w = display_screen->width_in_millimeters;
    int scr_mm_h = display_screen->height_in_millimeters;

    /* Expand screen if needed before setting CRTCs */
    if (max_x > scr_w || max_y > scr_h) {
        int new_w = max_x > scr_w ? max_x : scr_w;
        int new_h = max_y > scr_h ? max_y : scr_h;
        int mm_w = scr_w > 0 ? (new_w * scr_mm_w) / scr_w : new_w;
        int mm_h = scr_h > 0 ? (new_h * scr_mm_h) / scr_h : new_h;
        xcb_randr_set_screen_size(display_conn, display_root,
                                  new_w, new_h, mm_w, mm_h);
    }

    /* Apply each output's mode */
    for (int i = 0; i < noutputs; i++) {
        OutputInfo *o = &outputs[i];
        if (o->crtc_id == XCB_NONE || o->nmodes == 0) continue;
        ModeEntry *m = &o->modes[o->sel_mode_idx];
        xcb_randr_set_crtc_config(display_conn, o->crtc_id,
            XCB_CURRENT_TIME, cfg_ts,
            o->x, o->y, m->id,
            XCB_RANDR_ROTATION_ROTATE_0,
            1, &o->output_id);
    }

    /* Shrink screen if needed */
    if (max_x < scr_w || max_y < scr_h) {
        int mm_w = scr_w > 0 ? (max_x * scr_mm_w) / scr_w : max_x;
        int mm_h = scr_h > 0 ? (max_y * scr_mm_h) / scr_h : max_y;
        xcb_randr_set_screen_size(display_conn, display_root,
                                  max_x, max_y, mm_w, mm_h);
    }

    xcb_flush(display_conn);
    free(res);

    /* Save config and mark saved state */
    save_output_configs();
    for (int i = 0; i < noutputs; i++) {
        outputs[i].saved_mode_idx = outputs[i].sel_mode_idx;
        outputs[i].saved_scale = outputs[i].sel_scale;
    }

    if (display_dbus)
        isde_dbus_settings_notify(display_dbus, "display", "*");
}

static void display_revert(void)
{
    for (int i = 0; i < noutputs; i++) {
        outputs[i].sel_mode_idx = outputs[i].saved_mode_idx;
        outputs[i].sel_scale = outputs[i].saved_scale;
    }
    update_mode_combo();
    update_scale_slider();
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

    display_conn = IswDisplay(parent);
    display_screen = IswScreen(parent);
    display_root = display_screen->root;
    query_outputs(display_conn, display_root);
    load_output_configs();

    Widget prev = NULL;

    /* --- Output list --- */
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
    IswArgHeight(&ab, list_height);
    IswArgBorderWidth(&ab, 0);
    IswArgFromHoriz(&ab, out_lbl);
    IswArgLeft(&ab, IswChainLeft);
    output_list = IswCreateManagedWidget("outputList", listWidgetClass,
                                        form, ab.args, ab.count);
    IswAddCallback(output_list, IswNcallback, output_select_cb, NULL);

    selected_output = 0;
    for (int i = 0; i < noutputs; i++) {
        if (outputs[i].is_primary) { selected_output = i; break; }
    }
    IswListHighlight(output_list, selected_output);
    prev = output_list;

    /* --- Resolution combo --- */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Resolution:");
    IswArgBorderWidth(&ab, 0);
    IswArgFromVert(&ab, prev);
    IswArgWidth(&ab, LABEL_W);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    Widget res_lbl = IswCreateManagedWidget("lbl", labelWidgetClass,
                                            form, ab.args, ab.count);

    build_mode_strings(&outputs[selected_output]);

    IswArgBuilderReset(&ab);
    IswArgList(&ab, mode_strings);
    IswArgNumberStrings(&ab, nmode_strings);
    IswArgDefaultColumns(&ab, 1);
    IswArgForceColumns(&ab, True);
    IswArgVerticalList(&ab, True);
    IswArgBorderWidth(&ab, 0);
    IswArgFromVert(&ab, prev);
    IswArgFromHoriz(&ab, res_lbl);
    IswArgLeft(&ab, IswChainLeft);
    res_combo = IswCreateManagedWidget("resCombo", comboBoxWidgetClass,
                                      form, ab.args, ab.count);
    IswAddCallback(res_combo, IswNcallback, mode_select_cb, NULL);

    if (outputs[selected_output].sel_mode_idx >= 0)
        IswListHighlight(res_combo, outputs[selected_output].sel_mode_idx);
    prev = res_combo;

    /* --- HiDPI scale (per-output) --- */
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
    IswArgSliderValue(&ab, outputs[selected_output].sel_scale);
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
    for (int i = 0; i < noutputs; i++) {
        if (outputs[i].sel_mode_idx != outputs[i].saved_mode_idx) return 1;
        if (outputs[i].sel_scale != outputs[i].saved_scale) return 1;
    }
    return 0;
}

static void display_destroy(void)
{
    output_list = NULL;
    res_combo = NULL;
    scale_slider = NULL;
    scale_label = NULL;
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
