#define _POSIX_C_SOURCE 200809L
/*
 * panel-display.c — Display settings: output selection, resolution,
 *                   HiDPI scaling, enable/disable, layout, primary
 */
#include "settings.h"
#include <ISW/List.h>
#include <ISW/ComboBox.h>
#include <ISW/Slider.h>
#include <ISW/Toggle.h>
#include <ISW/DrawingArea.h>
#include <ISW/IswArgMacros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <cairo/cairo.h>
#include <xcb/xcb.h>
#include <xcb/randr.h>

#include "isde/isde-config.h"
#include "isde/isde-randr.h"

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
    int                  sel_enabled;
    int16_t              sel_x, sel_y;
    int                  sel_primary;

    int                  saved_mode_idx;
    int                  saved_scale;
    int                  saved_enabled;
    int16_t              saved_x, saved_y;
    int                  saved_primary;
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
static Widget enable_toggle;
static Widget primary_btn;
static Widget layout_canvas;

static int    selected_output;

static IsdeDBus *display_dbus;
static xcb_connection_t *display_conn;
static xcb_window_t      display_root;

static IswAppContext     display_app;
static IswIntervalId     randr_poll_id;
static xcb_timestamp_t   last_config_ts;

static int    drag_output = -1;
static int    drag_start_mx, drag_start_my;
static int    drag_start_ox, drag_start_oy;
static double drag_scale;

#define RANDR_POLL_MS 2000

static void randr_poll_cb(IswPointer, IswIntervalId *);

#define LABEL_W  150
#define LIST_W   300
#define SLIDER_W 300
#define CANVAS_H 200
#define LAYOUT_PAD 16
#define SNAP_THRESHOLD 32

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

static void normalize_positions(void)
{
    int min_x = INT_MAX, min_y = INT_MAX;
    for (int i = 0; i < noutputs; i++) {
        if (!outputs[i].sel_enabled) continue;
        if (outputs[i].sel_x < min_x) min_x = outputs[i].sel_x;
        if (outputs[i].sel_y < min_y) min_y = outputs[i].sel_y;
    }
    if (min_x == INT_MAX) return;
    for (int i = 0; i < noutputs; i++) {
        if (!outputs[i].sel_enabled) continue;
        outputs[i].sel_x -= min_x;
        outputs[i].sel_y -= min_y;
    }
}

static void request_canvas_redraw(void)
{
    if (!layout_canvas || !IswIsRealized(layout_canvas)) return;
    xcb_clear_area(display_conn, 1, IswWindow(layout_canvas), 0, 0, 0, 0);
    xcb_flush(display_conn);
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
            me->refresh = isde_randr_refresh(mi);
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
        int enabled = (oinfo->crtc != XCB_NONE);
        o->cur_width = 0;
        o->cur_height = 0;
        o->cur_mode = XCB_NONE;
        if (enabled) {
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
                o->cur_refresh = isde_randr_refresh(&mode_infos[k]);
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

        /* Initialize state fields */
        o->sel_enabled = enabled;
        o->saved_enabled = enabled;
        o->sel_x = o->x;
        o->sel_y = o->y;
        o->saved_x = o->x;
        o->saved_y = o->y;
        o->sel_primary = o->is_primary;
        o->saved_primary = o->is_primary;

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
        outputs[0].sel_enabled = 1;
        outputs[0].saved_enabled = 1;
        outputs[0].sel_primary = 1;
        outputs[0].saved_primary = 1;
        noutputs = 1;
    }

    /* Build display name strings */
    output_names = malloc((noutputs + 1) * sizeof(String));
    for (int i = 0; i < noutputs; i++) {
        char buf[128];
        OutputInfo *o = &outputs[i];
        uint16_t dw = o->sel_enabled && o->nmodes > 0 ?
            o->modes[o->sel_mode_idx].width : 0;
        uint16_t dh = o->sel_enabled && o->nmodes > 0 ?
            o->modes[o->sel_mode_idx].height : 0;
        snprintf(buf, sizeof(buf), "%s%s%s (%ux%u)",
                 o->name,
                 o->is_primary ? " [Primary]" : "",
                 o->sel_enabled ? "" : " [Off]",
                 dw, dh);
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

        int en = isde_config_bool(mon, "enabled", o->sel_enabled);
        o->sel_enabled = en;
        o->saved_enabled = en;

        o->sel_x = (int16_t)isde_config_int(mon, "x", o->x);
        o->sel_y = (int16_t)isde_config_int(mon, "y", o->y);
        o->saved_x = o->sel_x;
        o->saved_y = o->sel_y;

        int pri = isde_config_bool(mon, "primary", o->is_primary);
        o->sel_primary = pri;
        o->saved_primary = pri;
    }

    isde_config_free(cfg);
}

static void save_output_configs(void)
{
    char *path = isde_xdg_config_path("isde.toml");
    if (!path) return;

    for (int i = 0; i < noutputs; i++) {
        OutputInfo *o = &outputs[i];
        char section[128];
        snprintf(section, sizeof(section), "display.outputs.%s", o->name);

        isde_config_write_bool(path, section, "enabled", o->sel_enabled);

        if (o->sel_enabled && o->nmodes > 0) {
            ModeEntry *m = &o->modes[o->sel_mode_idx];
            isde_config_write_int(path, section, "width", m->width);
            isde_config_write_int(path, section, "height", m->height);
            isde_config_write_double(path, section, "refresh", m->refresh);
            isde_config_write_int(path, section, "x", o->sel_x);
            isde_config_write_int(path, section, "y", o->sel_y);
        }

        isde_config_write_int(path, section, "scale_percent", o->sel_scale);
        isde_config_write_bool(path, section, "primary", o->sel_primary);

        if (o->edid_hash)
            isde_config_write_string(path, section, "edid", o->edid_hash);
    }

    /* Global scale = primary output's scale */
    for (int i = 0; i < noutputs; i++) {
        if (outputs[i].sel_primary) {
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

/* ---------- UI update helpers ---------- */

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

static void update_enable_toggle(void)
{
    if (!enable_toggle || selected_output < 0 || selected_output >= noutputs)
        return;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgState(&ab, outputs[selected_output].sel_enabled ? True : False);
    IswSetValues(enable_toggle, ab.args, ab.count);
}

static void update_controls_sensitivity(void)
{
    if (selected_output < 0 || selected_output >= noutputs) return;
    Boolean sens = outputs[selected_output].sel_enabled ? True : False;

    if (res_combo) {
        IswArgBuilder a1 = IswArgBuilderInit();
        IswArgSensitive(&a1, sens);
        IswSetValues(res_combo, a1.args, a1.count);
    }
    if (scale_slider) {
        IswArgBuilder a2 = IswArgBuilderInit();
        IswArgSensitive(&a2, sens);
        IswSetValues(scale_slider, a2.args, a2.count);
    }
    if (primary_btn) {
        IswArgBuilder a3 = IswArgBuilderInit();
        IswArgSensitive(&a3, sens);
        IswSetValues(primary_btn, a3.args, a3.count);
    }
}

/* ---------- randr hotplug polling ---------- */

static void refresh_display_list(void)
{
    IswListChange(output_list, NULL, 0, 0, False);
    IswListChange(res_combo, NULL, 0, 0, False);

    char *sel_name = NULL;
    if (selected_output >= 0 && selected_output < noutputs)
        sel_name = strdup(outputs[selected_output].name);

    query_outputs(display_conn, display_root);
    load_output_configs();

    IswListChange(output_list, output_names, noutputs, 0, True);

    selected_output = 0;
    if (sel_name) {
        for (int i = 0; i < noutputs; i++) {
            if (strcmp(outputs[i].name, sel_name) == 0) {
                selected_output = i;
                break;
            }
        }
        free(sel_name);
    }
    if (selected_output == 0) {
        for (int i = 0; i < noutputs; i++) {
            if (outputs[i].is_primary) { selected_output = i; break; }
        }
    }

    IswListHighlight(output_list, selected_output);
    update_mode_combo();
    update_scale_slider();
    update_enable_toggle();
    update_controls_sensitivity();
    request_canvas_redraw();
}

static void randr_poll_cb(IswPointer client_data, IswIntervalId *id)
{
    (void)client_data;
    (void)id;

    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(display_conn,
            xcb_randr_get_screen_resources_current(display_conn, display_root),
            NULL);
    if (res) {
        if (res->config_timestamp != last_config_ts) {
            last_config_ts = res->config_timestamp;
            refresh_display_list();
        }
        free(res);
    }

    randr_poll_id = IswAppAddTimeOut(display_app, RANDR_POLL_MS,
                                    randr_poll_cb, NULL);
}

/* ---------- layout transform ---------- */

typedef struct {
    int    bb_x0, bb_y0;
    double scale;
    int    off_x, off_y;
    int    valid;
} LayoutTransform;

static LayoutTransform compute_layout_transform(Dimension cw, Dimension ch)
{
    LayoutTransform lt = { .valid = 0 };

    int bb_x0 = 0, bb_y0 = 0, bb_x1 = 1, bb_y1 = 1;
    int first = 1;
    for (int i = 0; i < noutputs; i++) {
        if (!outputs[i].sel_enabled || outputs[i].nmodes == 0) continue;
        ModeEntry *m = &outputs[i].modes[outputs[i].sel_mode_idx];
        int x0 = outputs[i].sel_x, y0 = outputs[i].sel_y;
        int x1 = x0 + m->width, y1 = y0 + m->height;
        if (first || x0 < bb_x0) bb_x0 = x0;
        if (first || y0 < bb_y0) bb_y0 = y0;
        if (first || x1 > bb_x1) bb_x1 = x1;
        if (first || y1 > bb_y1) bb_y1 = y1;
        first = 0;
    }
    if (first) return lt;

    int bb_w = bb_x1 - bb_x0, bb_h = bb_y1 - bb_y0;
    if (bb_w <= 0 || bb_h <= 0) return lt;

    int avail_w = cw - 2 * LAYOUT_PAD;
    int avail_h = ch - 2 * LAYOUT_PAD;
    if (avail_w <= 0 || avail_h <= 0) return lt;

    double scale = (double)avail_w / bb_w;
    if ((double)avail_h / bb_h < scale)
        scale = (double)avail_h / bb_h;

    lt.bb_x0 = bb_x0;
    lt.bb_y0 = bb_y0;
    lt.scale = scale;
    lt.off_x = LAYOUT_PAD + (avail_w - (int)(bb_w * scale)) / 2;
    lt.off_y = LAYOUT_PAD + (avail_h - (int)(bb_h * scale)) / 2;
    lt.valid = 1;
    return lt;
}

/* ---------- layout canvas callbacks ---------- */

static void layout_expose_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)cd;
    ISWDrawingCallbackData *d = (ISWDrawingCallbackData *)call;
    cairo_t *cr = (cairo_t *)ISWRenderGetCairoContext(d->render_ctx);
    if (!cr) return;

    const IsdeColorScheme *scheme = isde_theme_current();

    Dimension cw, ch;
    IswArgBuilder qb = IswArgBuilderInit();
    IswArgWidth(&qb, &cw);
    IswArgHeight(&qb, &ch);
    IswGetValues(w, qb.args, qb.count);

    /* Background */
    double r, g, b;
    isde_color_to_rgb(scheme ? scheme->bg : 0x333333, &r, &g, &b);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_paint(cr);

    LayoutTransform lt = compute_layout_transform(cw, ch);
    if (!lt.valid) return;

    for (int i = 0; i < noutputs; i++) {
        if (!outputs[i].sel_enabled || outputs[i].nmodes == 0) continue;
        ModeEntry *m = &outputs[i].modes[outputs[i].sel_mode_idx];

        int rx = lt.off_x + (int)((outputs[i].sel_x - lt.bb_x0) * lt.scale);
        int ry = lt.off_y + (int)((outputs[i].sel_y - lt.bb_y0) * lt.scale);
        int rw = (int)(m->width * lt.scale);
        int rh = (int)(m->height * lt.scale);
        if (rw < 2) rw = 2;
        if (rh < 2) rh = 2;

        /* Fill */
        unsigned int fill_color;
        if (i == selected_output)
            fill_color = scheme ? scheme->active : 0x4488CC;
        else
            fill_color = scheme ? scheme->bg_light : 0x555555;
        isde_color_to_rgb(fill_color, &r, &g, &b);
        cairo_set_source_rgb(cr, r, g, b);
        cairo_rectangle(cr, rx, ry, rw, rh);
        cairo_fill(cr);

        /* Border */
        isde_color_to_rgb(scheme ? scheme->border : 0x888888, &r, &g, &b);
        cairo_set_source_rgb(cr, r, g, b);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, rx + 0.5, ry + 0.5, rw - 1, rh - 1);
        cairo_stroke(cr);

        /* Label */
        isde_color_to_rgb(scheme ? scheme->fg_light : 0xFFFFFF, &r, &g, &b);
        cairo_set_source_rgb(cr, r, g, b);
        cairo_select_font_face(cr, "sans-serif",
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.0);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, outputs[i].name, &ext);
        int tx = rx + (rw - (int)ext.width) / 2;
        int ty = ry + (rh + (int)ext.height) / 2;
        cairo_move_to(cr, tx, ty);
        cairo_show_text(cr, outputs[i].name);

        if (outputs[i].sel_primary) {
            cairo_set_font_size(cr, 9.0);
            cairo_move_to(cr, rx + 4, ry + 12);
            cairo_show_text(cr, "P");
        }
    }
}

static void snap_output_edges(int idx)
{
    OutputInfo *o = &outputs[idx];
    if (!o->sel_enabled || o->nmodes == 0) return;
    ModeEntry *m = &o->modes[o->sel_mode_idx];
    int ox0 = o->sel_x, oy0 = o->sel_y;
    int ox1 = ox0 + m->width, oy1 = oy0 + m->height;

    int best_dx = SNAP_THRESHOLD + 1;
    int best_dy = SNAP_THRESHOLD + 1;

    for (int i = 0; i < noutputs; i++) {
        if (i == idx || !outputs[i].sel_enabled || outputs[i].nmodes == 0)
            continue;
        ModeEntry *m2 = &outputs[i].modes[outputs[i].sel_mode_idx];
        int tx0 = outputs[i].sel_x, ty0 = outputs[i].sel_y;
        int tx1 = tx0 + m2->width, ty1 = ty0 + m2->height;

        int cx[] = { tx0 - ox0, tx1 - ox1, tx1 - ox0, tx0 - ox1 };
        for (int c = 0; c < 4; c++)
            if (abs(cx[c]) < abs(best_dx)) best_dx = cx[c];

        int cy[] = { ty0 - oy0, ty1 - oy1, ty1 - oy0, ty0 - oy1 };
        for (int c = 0; c < 4; c++)
            if (abs(cy[c]) < abs(best_dy)) best_dy = cy[c];
    }

    if (abs(ox0) < abs(best_dx)) best_dx = -ox0;
    if (abs(oy0) < abs(best_dy)) best_dy = -oy0;

    if (abs(best_dx) <= SNAP_THRESHOLD)
        o->sel_x += best_dx;
    if (abs(best_dy) <= SNAP_THRESHOLD)
        o->sel_y += best_dy;
}

static void layout_input_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)cd;
    ISWDrawingCallbackData *d = (ISWDrawingCallbackData *)call;
    if (!d->event) return;

    uint8_t type = d->event->response_type & ~0x80;

    Dimension cw, ch;
    IswArgBuilder qb = IswArgBuilderInit();
    IswArgWidth(&qb, &cw);
    IswArgHeight(&qb, &ch);
    IswGetValues(w, qb.args, qb.count);

    LayoutTransform lt = compute_layout_transform(cw, ch);

    if (type == XCB_BUTTON_PRESS) {
        xcb_button_press_event_t *bp = (xcb_button_press_event_t *)d->event;
        if (bp->detail != 1) return;
        if (!lt.valid) return;

        drag_output = -1;
        for (int i = noutputs - 1; i >= 0; i--) {
            if (!outputs[i].sel_enabled || outputs[i].nmodes == 0) continue;
            ModeEntry *m = &outputs[i].modes[outputs[i].sel_mode_idx];
            int rx = lt.off_x + (int)((outputs[i].sel_x - lt.bb_x0) * lt.scale);
            int ry = lt.off_y + (int)((outputs[i].sel_y - lt.bb_y0) * lt.scale);
            int rw = (int)(m->width * lt.scale);
            int rh = (int)(m->height * lt.scale);
            if (bp->event_x >= rx && bp->event_x < rx + rw &&
                bp->event_y >= ry && bp->event_y < ry + rh) {
                drag_output = i;
                drag_start_mx = bp->event_x;
                drag_start_my = bp->event_y;
                drag_start_ox = outputs[i].sel_x;
                drag_start_oy = outputs[i].sel_y;
                drag_scale = lt.scale;

                if (i != selected_output) {
                    selected_output = i;
                    IswListHighlight(output_list, selected_output);
                    update_mode_combo();
                    update_scale_slider();
                    update_enable_toggle();
                    update_controls_sensitivity();
                }
                break;
            }
        }
    } else if (type == XCB_MOTION_NOTIFY && drag_output >= 0) {
        xcb_motion_notify_event_t *mn = (xcb_motion_notify_event_t *)d->event;
        if (drag_scale <= 0) return;
        int dx = (int)((mn->event_x - drag_start_mx) / drag_scale);
        int dy = (int)((mn->event_y - drag_start_my) / drag_scale);
        outputs[drag_output].sel_x = drag_start_ox + dx;
        outputs[drag_output].sel_y = drag_start_oy + dy;
        request_canvas_redraw();
    } else if (type == XCB_BUTTON_RELEASE && drag_output >= 0) {
        snap_output_edges(drag_output);
        normalize_positions();
        drag_output = -1;
        request_canvas_redraw();
    }
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
        update_enable_toggle();
        update_controls_sensitivity();
        request_canvas_redraw();
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
        request_canvas_redraw();
    }
}

static void scale_changed_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)cd;
    int *val = (int *)call;
    if (selected_output >= 0 && selected_output < noutputs)
        outputs[selected_output].sel_scale = *val;
}

static void enable_changed_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)cd; (void)call;
    if (selected_output < 0 || selected_output >= noutputs) return;

    Boolean state = False;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgState(&ab, &state);
    IswGetValues(enable_toggle, ab.args, ab.count);

    outputs[selected_output].sel_enabled = state ? 1 : 0;
    update_controls_sensitivity();
    request_canvas_redraw();
}

static void primary_clicked_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)cd; (void)call;
    if (selected_output < 0 || selected_output >= noutputs) return;
    for (int i = 0; i < noutputs; i++)
        outputs[i].sel_primary = 0;
    outputs[selected_output].sel_primary = 1;
    request_canvas_redraw();
}

/* ---------- apply: save config and signal daemon ---------- */

static void display_apply(void)
{
    normalize_positions();

    save_output_configs();
    for (int i = 0; i < noutputs; i++) {
        outputs[i].saved_mode_idx = outputs[i].sel_mode_idx;
        outputs[i].saved_scale = outputs[i].sel_scale;
        outputs[i].saved_enabled = outputs[i].sel_enabled;
        outputs[i].saved_x = outputs[i].sel_x;
        outputs[i].saved_y = outputs[i].sel_y;
        outputs[i].saved_primary = outputs[i].sel_primary;
    }

    if (display_dbus)
        isde_dbus_settings_notify(display_dbus, "display", "*");
}

static void display_revert(void)
{
    for (int i = 0; i < noutputs; i++) {
        outputs[i].sel_mode_idx = outputs[i].saved_mode_idx;
        outputs[i].sel_scale = outputs[i].saved_scale;
        outputs[i].sel_enabled = outputs[i].saved_enabled;
        outputs[i].sel_x = outputs[i].saved_x;
        outputs[i].sel_y = outputs[i].saved_y;
        outputs[i].sel_primary = outputs[i].saved_primary;
    }
    update_mode_combo();
    update_scale_slider();
    update_enable_toggle();
    update_controls_sensitivity();
    request_canvas_redraw();
}

/* ---------- create ---------- */

static Widget display_create(Widget parent, IswAppContext app)
{
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

    /* --- Enable toggle + Primary button --- */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Enabled:");
    IswArgBorderWidth(&ab, 0);
    IswArgFromVert(&ab, prev);
    IswArgWidth(&ab, LABEL_W);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    Widget en_lbl = IswCreateManagedWidget("lbl", labelWidgetClass,
                                           form, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "");
    IswArgBorderWidth(&ab, 1);
    IswArgFromVert(&ab, prev);
    IswArgFromHoriz(&ab, en_lbl);
    IswArgLeft(&ab, IswChainLeft);
    if (outputs[selected_output].sel_enabled)
        IswArgState(&ab, True);
    enable_toggle = IswCreateManagedWidget("enableToggle", toggleWidgetClass,
                                           form, ab.args, ab.count);
    IswAddCallback(enable_toggle, IswNcallback, enable_changed_cb, NULL);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Set as Primary");
    IswArgBorderWidth(&ab, 1);
    IswArgFromVert(&ab, prev);
    IswArgFromHoriz(&ab, enable_toggle);
    IswArgLeft(&ab, IswChainLeft);
    IswArgWidth(&ab, 120);
    primary_btn = IswCreateManagedWidget("primaryBtn", commandWidgetClass,
                                         form, ab.args, ab.count);
    IswAddCallback(primary_btn, IswNcallback, primary_clicked_cb, NULL);
    prev = enable_toggle;

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

    /* --- Layout preview --- */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Layout:");
    IswArgBorderWidth(&ab, 0);
    IswArgFromVert(&ab, prev);
    IswArgWidth(&ab, LABEL_W);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    Widget lay_lbl = IswCreateManagedWidget("lbl", labelWidgetClass,
                                            form, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgFromVert(&ab, prev);
    IswArgFromHoriz(&ab, lay_lbl);
    IswArgWidth(&ab, SLIDER_W);
    IswArgHeight(&ab, CANVAS_H);
    IswArgBorderWidth(&ab, 1);
    IswArgLeft(&ab, IswChainLeft);
    layout_canvas = IswCreateManagedWidget("layoutCanvas",
                                           drawingAreaWidgetClass,
                                           form, ab.args, ab.count);
    IswAddCallback(layout_canvas, IswNexposeCallback, layout_expose_cb, NULL);
    IswAddCallback(layout_canvas, IswNinputCallback, layout_input_cb, NULL);
    prev = layout_canvas;

    update_controls_sensitivity();

    display_app = app;
    xcb_randr_select_input(display_conn, display_root,
        XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE |
        XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE);
    xcb_flush(display_conn);

    xcb_randr_get_screen_resources_current_reply_t *ts_res =
        xcb_randr_get_screen_resources_current_reply(display_conn,
            xcb_randr_get_screen_resources_current(display_conn, display_root),
            NULL);
    if (ts_res) {
        last_config_ts = ts_res->config_timestamp;
        free(ts_res);
    }
    randr_poll_id = IswAppAddTimeOut(display_app, RANDR_POLL_MS,
                                    randr_poll_cb, NULL);

    return form;
}

static int display_has_changes(void)
{
    for (int i = 0; i < noutputs; i++) {
        if (outputs[i].sel_mode_idx != outputs[i].saved_mode_idx) return 1;
        if (outputs[i].sel_scale != outputs[i].saved_scale) return 1;
        if (outputs[i].sel_enabled != outputs[i].saved_enabled) return 1;
        if (outputs[i].sel_x != outputs[i].saved_x) return 1;
        if (outputs[i].sel_y != outputs[i].saved_y) return 1;
        if (outputs[i].sel_primary != outputs[i].saved_primary) return 1;
    }
    return 0;
}

static void display_destroy(void)
{
    if (randr_poll_id) {
        IswRemoveTimeOut(randr_poll_id);
        randr_poll_id = 0;
    }
    output_list = NULL;
    res_combo = NULL;
    scale_slider = NULL;
    scale_label = NULL;
    enable_toggle = NULL;
    primary_btn = NULL;
    layout_canvas = NULL;
    drag_output = -1;
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
