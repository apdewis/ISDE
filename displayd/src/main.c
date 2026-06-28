#define _POSIX_C_SOURCE 200809L
/*
 * isde-displayd — display auto-configuration daemon
 *
 * Three distinct operations:
 *   disable_disconnected() — release CRTCs for physically absent outputs
 *   enable_connected()     — configure any connected output lacking a CRTC
 *   apply_config()         — read isde.toml and set modes/positions/primary
 *
 * Event sources:
 *   udev DRM hotplug  → disable_disconnected, then enable_connected
 *   RandR notify      → disable_disconnected, then enable_connected
 *   D-Bus / SIGHUP    → apply_config (user changed settings)
 *   startup           → apply_config
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <poll.h>
#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <xcb/dpms.h>
#include <libudev.h>

#include "isde-config.h"
#include "isde-xdg.h"
#include "dbus.h"
#include "randr.h"

static volatile sig_atomic_t reload_flag;
static volatile sig_atomic_t quit_flag;
static volatile sig_atomic_t dbus_reload_flag;

static void on_sighup(int sig)  { (void)sig; reload_flag = 1; }
static void on_sigterm(int sig) { (void)sig; quit_flag = 1; }

static void on_settings_changed(const char *section, const char *key,
                                 void *user_data)
{
    (void)key; (void)user_data;
    if (strcmp(section, "display") == 0 || strcmp(section, "*") == 0)
        dbus_reload_flag = 1;
}

/* ---------- disable_disconnected ----------
 * Release CRTCs for outputs that are no longer physically present. */

static void disable_disconnected(xcb_connection_t *conn, xcb_window_t root)
{
    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, root), NULL);
    if (!res) return;

    xcb_timestamp_t cfg_ts = res->config_timestamp;
    xcb_randr_output_t *outs =
        xcb_randr_get_screen_resources_current_outputs(res);
    int nouts = xcb_randr_get_screen_resources_current_outputs_length(res);

    for (int i = 0; i < nouts; i++) {
        xcb_randr_get_output_info_reply_t *oi =
            xcb_randr_get_output_info_reply(conn,
                xcb_randr_get_output_info(conn, outs[i], cfg_ts), NULL);
        if (!oi) continue;
        if (oi->connection == XCB_RANDR_CONNECTION_CONNECTED ||
            oi->crtc == XCB_NONE) {
            free(oi);
            continue;
        }
        int namelen = xcb_randr_get_output_info_name_length(oi);
        char *name = strndup((char *)xcb_randr_get_output_info_name(oi),
                             namelen);
        xcb_randr_set_crtc_config(conn, oi->crtc,
            XCB_CURRENT_TIME, cfg_ts,
            0, 0, XCB_NONE,
            XCB_RANDR_ROTATION_ROTATE_0, 0, NULL);
        fprintf(stderr, "isde-displayd: released CRTC for disconnected %s\n",
                name);
        free(name);
        free(oi);
    }

    xcb_flush(conn);
    free(res);
}

/* ---------- enable_connected ----------
 * For each connected output that has no CRTC, assign one and set
 * the preferred mode.  Reads isde.toml for per-output overrides. */

static void enable_connected(xcb_connection_t *conn, xcb_window_t root,
                              xcb_screen_t *scr)
{
    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, root), NULL);
    if (!res) return;

    xcb_randr_mode_info_t *mode_infos =
        xcb_randr_get_screen_resources_current_modes(res);
    int nmi = xcb_randr_get_screen_resources_current_modes_length(res);
    xcb_timestamp_t cfg_ts = res->config_timestamp;
    xcb_randr_output_t *outs =
        xcb_randr_get_screen_resources_current_outputs(res);
    int nouts = xcb_randr_get_screen_resources_current_outputs_length(res);

    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    IsdeConfigTable *outs_tbl = NULL;
    if (cfg) {
        IsdeConfigTable *cfg_root = isde_config_root(cfg);
        IsdeConfigTable *disp = isde_config_table(cfg_root, "display");
        if (disp)
            outs_tbl = isde_config_table(disp, "outputs");
    }

    xcb_randr_output_t first_enabled = XCB_NONE;

    for (int i = 0; i < nouts; i++) {
        xcb_randr_get_output_info_reply_t *oi =
            xcb_randr_get_output_info_reply(conn,
                xcb_randr_get_output_info(conn, outs[i], cfg_ts), NULL);
        if (!oi) continue;
        if (oi->connection != XCB_RANDR_CONNECTION_CONNECTED) {
            free(oi);
            continue;
        }
        if (oi->crtc != XCB_NONE) {
            if (first_enabled == XCB_NONE)
                first_enabled = outs[i];
            free(oi);
            continue;
        }

        int namelen = xcb_randr_get_output_info_name_length(oi);
        char *name = strndup((char *)xcb_randr_get_output_info_name(oi),
                             namelen);

        IsdeConfigTable *mon = outs_tbl ?
            isde_config_table(outs_tbl, name) : NULL;

        if (mon && !isde_config_bool(mon, "enabled", 1)) {
            free(name);
            free(oi);
            continue;
        }

        int want_w = mon ? (int)isde_config_int(mon, "width", 0) : 0;
        int want_h = mon ? (int)isde_config_int(mon, "height", 0) : 0;
        int want_x = mon ? (int)isde_config_int(mon, "x", 0) : 0;
        int want_y = mon ? (int)isde_config_int(mon, "y", 0) : 0;

        if (want_w <= 0 || want_h <= 0) {
            xcb_randr_mode_t *pref_modes =
                xcb_randr_get_output_info_modes(oi);
            if (xcb_randr_get_output_info_modes_length(oi) > 0 &&
                oi->num_preferred > 0) {
                for (int k = 0; k < nmi; k++) {
                    if (mode_infos[k].id == pref_modes[0]) {
                        want_w = mode_infos[k].width;
                        want_h = mode_infos[k].height;
                        break;
                    }
                }
            }
        }

        if (want_w <= 0 || want_h <= 0) {
            free(name);
            free(oi);
            continue;
        }

        xcb_randr_crtc_t crtc = isde_randr_find_free_crtc(conn, oi, cfg_ts);
        if (crtc == XCB_NONE) {
            fprintf(stderr, "isde-displayd: no free CRTC for %s\n", name);
            free(name);
            free(oi);
            continue;
        }

        xcb_randr_mode_t *out_modes = xcb_randr_get_output_info_modes(oi);
        int out_nmodes = xcb_randr_get_output_info_modes_length(oi);
        xcb_randr_mode_t best_mode = XCB_NONE;
        double best_refresh = 0;

        for (int m = 0; m < out_nmodes; m++) {
            for (int k = 0; k < nmi; k++) {
                if (mode_infos[k].id != out_modes[m]) continue;
                if (mode_infos[k].width == (uint16_t)want_w &&
                    mode_infos[k].height == (uint16_t)want_h) {
                    double r = isde_randr_refresh(&mode_infos[k]);
                    if (r > best_refresh) {
                        best_refresh = r;
                        best_mode = mode_infos[k].id;
                    }
                }
            }
        }

        if (best_mode == XCB_NONE) {
            fprintf(stderr, "isde-displayd: no mode %dx%d for %s\n",
                    want_w, want_h, name);
            free(name);
            free(oi);
            continue;
        }

        xcb_get_geometry_reply_t *rg =
            xcb_get_geometry_reply(conn,
                xcb_get_geometry(conn, root), NULL);
        uint16_t cur_sw = rg ? rg->width  : scr->width_in_pixels;
        uint16_t cur_sh = rg ? rg->height : scr->height_in_pixels;
        free(rg);
        int right = want_x + want_w;
        int bottom = want_y + want_h;
        if (right > (int)cur_sw || bottom > (int)cur_sh) {
            int nw = right  > (int)cur_sw ? right  : (int)cur_sw;
            int nh = bottom > (int)cur_sh ? bottom : (int)cur_sh;
            int mm_w = scr->width_in_pixels > 0 ?
                (nw * scr->width_in_millimeters) / scr->width_in_pixels : nw;
            int mm_h = scr->height_in_pixels > 0 ?
                (nh * scr->height_in_millimeters) / scr->height_in_pixels : nh;
            xcb_randr_set_screen_size(conn, root, nw, nh, mm_w, mm_h);
        }

        {
            xcb_randr_get_screen_resources_current_reply_t *fresh =
                xcb_randr_get_screen_resources_current_reply(conn,
                    xcb_randr_get_screen_resources_current(conn, root), NULL);
            if (fresh) {
                cfg_ts = fresh->config_timestamp;
                free(fresh);
            }
        }

        xcb_randr_set_crtc_config_cookie_t ck =
            xcb_randr_set_crtc_config(conn, crtc,
                XCB_CURRENT_TIME, cfg_ts,
                (int16_t)want_x, (int16_t)want_y, best_mode,
                XCB_RANDR_ROTATION_ROTATE_0,
                1, &outs[i]);
        xcb_randr_set_crtc_config_reply_t *cr =
            xcb_randr_set_crtc_config_reply(conn, ck, NULL);
        if (!cr || cr->status != XCB_RANDR_SET_CONFIG_SUCCESS) {
            fprintf(stderr, "isde-displayd: set_crtc_config failed for %s"
                    " (status %d)\n", name, cr ? cr->status : -1);
        } else {
            fprintf(stderr, "isde-displayd: enabled %s -> %dx%d+%d+%d"
                    " @ %.0f Hz\n",
                    name, want_w, want_h, want_x, want_y, best_refresh);
            if (first_enabled == XCB_NONE)
                first_enabled = outs[i];
        }
        free(cr);
        free(name);
        free(oi);
    }

    if (first_enabled != XCB_NONE)
        xcb_randr_set_output_primary(conn, root, first_enabled);

    xcb_flush(conn);
    free(res);
    isde_config_free(cfg);
}

/* ---------- apply_config ----------
 * Full config application from isde.toml: disable outputs marked off,
 * set modes/positions for all enabled outputs.  Used on startup,
 * SIGHUP, and settings-panel changes. */

static void apply_config(xcb_connection_t *conn, xcb_window_t root,
                          xcb_screen_t *scr)
{
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (!cfg) return;

    IsdeConfigTable *cfg_root = isde_config_root(cfg);
    IsdeConfigTable *disp = isde_config_table(cfg_root, "display");
    if (!disp) { isde_config_free(cfg); return; }
    IsdeConfigTable *outs_tbl = isde_config_table(disp, "outputs");
    if (!outs_tbl) { isde_config_free(cfg); return; }

    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, root), NULL);
    if (!res) { isde_config_free(cfg); return; }

    xcb_randr_mode_info_t *mode_infos =
        xcb_randr_get_screen_resources_current_modes(res);
    int nmi = xcb_randr_get_screen_resources_current_modes_length(res);
    xcb_timestamp_t cfg_ts = res->config_timestamp;

    xcb_randr_output_t *randr_outs =
        xcb_randr_get_screen_resources_current_outputs(res);
    int n_randr_outs =
        xcb_randr_get_screen_resources_current_outputs_length(res);

    xcb_randr_output_t primary_out = XCB_NONE;
    xcb_randr_output_t first_enabled = XCB_NONE;
    int changed = 0;

    /* Disable outputs that config says should be off */
    for (int i = 0; i < n_randr_outs; i++) {
        xcb_randr_get_output_info_reply_t *oinfo =
            xcb_randr_get_output_info_reply(conn,
                xcb_randr_get_output_info(conn, randr_outs[i], cfg_ts), NULL);
        if (!oinfo) continue;
        if (oinfo->connection != XCB_RANDR_CONNECTION_CONNECTED) {
            free(oinfo);
            continue;
        }

        int namelen = xcb_randr_get_output_info_name_length(oinfo);
        char *name = strndup((char *)xcb_randr_get_output_info_name(oinfo),
                             namelen);

        IsdeConfigTable *mon = isde_config_table(outs_tbl, name);
        if (mon && !isde_config_bool(mon, "enabled", 1) &&
            oinfo->crtc != XCB_NONE) {
            xcb_randr_set_crtc_config(conn, oinfo->crtc,
                XCB_CURRENT_TIME, cfg_ts,
                0, 0, XCB_NONE,
                XCB_RANDR_ROTATION_ROTATE_0, 0, NULL);
            fprintf(stderr, "isde-displayd: disabled %s\n", name);
            changed = 1;
        }

        free(name);
        free(oinfo);
    }

    if (changed) {
        free(res);
        res = xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, root), NULL);
        if (!res) { isde_config_free(cfg); return; }
        mode_infos = xcb_randr_get_screen_resources_current_modes(res);
        nmi = xcb_randr_get_screen_resources_current_modes_length(res);
        cfg_ts = res->config_timestamp;
        randr_outs = xcb_randr_get_screen_resources_current_outputs(res);
        n_randr_outs = xcb_randr_get_screen_resources_current_outputs_length(res);
    }

    /* Configure all enabled connected outputs */
    for (int i = 0; i < n_randr_outs; i++) {
        xcb_randr_get_output_info_reply_t *oinfo =
            xcb_randr_get_output_info_reply(conn,
                xcb_randr_get_output_info(conn, randr_outs[i], cfg_ts), NULL);
        if (!oinfo) continue;
        if (oinfo->connection != XCB_RANDR_CONNECTION_CONNECTED) {
            free(oinfo);
            continue;
        }

        int namelen = xcb_randr_get_output_info_name_length(oinfo);
        char *name = strndup((char *)xcb_randr_get_output_info_name(oinfo),
                             namelen);

        IsdeConfigTable *mon = isde_config_table(outs_tbl, name);
        if (mon && !isde_config_bool(mon, "enabled", 1)) {
            free(name);
            free(oinfo);
            continue;
        }

        int want_w = mon ? (int)isde_config_int(mon, "width", 0) : 0;
        int want_h = mon ? (int)isde_config_int(mon, "height", 0) : 0;
        int want_x = mon ? (int)isde_config_int(mon, "x", 0) : 0;
        int want_y = mon ? (int)isde_config_int(mon, "y", 0) : 0;
        int is_primary = mon ? isde_config_bool(mon, "primary", 0) : 0;

        if (want_w <= 0 || want_h <= 0) {
            xcb_randr_mode_t *pref_modes =
                xcb_randr_get_output_info_modes(oinfo);
            if (xcb_randr_get_output_info_modes_length(oinfo) > 0 &&
                oinfo->num_preferred > 0) {
                for (int k = 0; k < nmi; k++) {
                    if (mode_infos[k].id == pref_modes[0]) {
                        want_w = mode_infos[k].width;
                        want_h = mode_infos[k].height;
                        break;
                    }
                }
            }
        }

        if (want_w <= 0 || want_h <= 0) {
            if (oinfo->crtc != XCB_NONE && first_enabled == XCB_NONE)
                first_enabled = randr_outs[i];
            free(name);
            free(oinfo);
            continue;
        }

        xcb_randr_crtc_t crtc = oinfo->crtc;
        if (crtc == XCB_NONE) {
            crtc = isde_randr_find_free_crtc(conn, oinfo, cfg_ts);
            if (crtc == XCB_NONE) {
                fprintf(stderr, "isde-displayd: no free CRTC for %s\n", name);
                free(name);
                free(oinfo);
                continue;
            }
        }

        /* Skip if already configured correctly */
        int already_correct = 0;
        if (oinfo->crtc != XCB_NONE) {
            xcb_randr_get_crtc_info_reply_t *ci =
                xcb_randr_get_crtc_info_reply(conn,
                    xcb_randr_get_crtc_info(conn, crtc, cfg_ts), NULL);
            if (ci) {
                if (ci->mode != XCB_NONE &&
                    ci->x == (int16_t)want_x && ci->y == (int16_t)want_y &&
                    ci->width == (uint16_t)want_w &&
                    ci->height == (uint16_t)want_h)
                    already_correct = 1;
                free(ci);
            }
        }

        if (already_correct) {
            if (first_enabled == XCB_NONE)
                first_enabled = randr_outs[i];
            if (is_primary)
                primary_out = randr_outs[i];
            free(name);
            free(oinfo);
            continue;
        }

        xcb_randr_mode_t *out_modes = xcb_randr_get_output_info_modes(oinfo);
        int out_nmodes = xcb_randr_get_output_info_modes_length(oinfo);
        xcb_randr_mode_t best_mode = XCB_NONE;
        double best_refresh = 0;

        for (int m = 0; m < out_nmodes; m++) {
            for (int k = 0; k < nmi; k++) {
                if (mode_infos[k].id != out_modes[m]) continue;
                if (mode_infos[k].width == (uint16_t)want_w &&
                    mode_infos[k].height == (uint16_t)want_h) {
                    double r = isde_randr_refresh(&mode_infos[k]);
                    if (r > best_refresh) {
                        best_refresh = r;
                        best_mode = mode_infos[k].id;
                    }
                }
            }
        }

        if (best_mode == XCB_NONE) {
            fprintf(stderr, "isde-displayd: no mode %dx%d for %s\n",
                    want_w, want_h, name);
            free(name);
            free(oinfo);
            continue;
        }

        xcb_get_geometry_reply_t *rg =
            xcb_get_geometry_reply(conn,
                xcb_get_geometry(conn, root), NULL);
        uint16_t cur_sw = rg ? rg->width  : scr->width_in_pixels;
        uint16_t cur_sh = rg ? rg->height : scr->height_in_pixels;
        free(rg);
        int right = want_x + want_w;
        int bottom = want_y + want_h;
        if (right > (int)cur_sw || bottom > (int)cur_sh) {
            int nw = right  > (int)cur_sw ? right  : (int)cur_sw;
            int nh = bottom > (int)cur_sh ? bottom : (int)cur_sh;
            int mm_w = scr->width_in_pixels > 0 ?
                (nw * scr->width_in_millimeters) / scr->width_in_pixels : nw;
            int mm_h = scr->height_in_pixels > 0 ?
                (nh * scr->height_in_millimeters) / scr->height_in_pixels : nh;
            xcb_randr_set_screen_size(conn, root, nw, nh, mm_w, mm_h);
        }

        {
            xcb_randr_get_screen_resources_current_reply_t *fresh =
                xcb_randr_get_screen_resources_current_reply(conn,
                    xcb_randr_get_screen_resources_current(conn, root), NULL);
            if (fresh) {
                cfg_ts = fresh->config_timestamp;
                free(fresh);
            }
        }
        xcb_randr_set_crtc_config_cookie_t ck =
            xcb_randr_set_crtc_config(conn, crtc,
                XCB_CURRENT_TIME, cfg_ts,
                (int16_t)want_x, (int16_t)want_y, best_mode,
                XCB_RANDR_ROTATION_ROTATE_0,
                1, &randr_outs[i]);
        xcb_randr_set_crtc_config_reply_t *cr =
            xcb_randr_set_crtc_config_reply(conn, ck, NULL);
        if (!cr || cr->status != XCB_RANDR_SET_CONFIG_SUCCESS) {
            fprintf(stderr, "isde-displayd: set_crtc_config failed for %s"
                    " (status %d)\n", name, cr ? cr->status : -1);
            free(cr);
            free(name);
            free(oinfo);
            continue;
        }
        free(cr);
        fprintf(stderr, "isde-displayd: %s -> %dx%d+%d+%d @ %.0f Hz\n",
                name, want_w, want_h, want_x, want_y, best_refresh);

        if (first_enabled == XCB_NONE)
            first_enabled = randr_outs[i];
        if (is_primary)
            primary_out = randr_outs[i];

        free(name);
        free(oinfo);
    }

    xcb_randr_output_t new_primary =
        primary_out != XCB_NONE ? primary_out : first_enabled;
    if (new_primary != XCB_NONE)
        xcb_randr_set_output_primary(conn, root, new_primary);

    xcb_flush(conn);
    free(res);
    isde_config_free(cfg);
}

/* ---------- main ---------- */

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    struct sigaction sa = { .sa_handler = on_sighup };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGHUP, &sa, NULL);
    sa.sa_handler = on_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    const char *display = getenv("DISPLAY");
    int screen_num;
    xcb_connection_t *conn = xcb_connect(display, &screen_num);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "isde-displayd: cannot connect to X\n");
        return 1;
    }

    xcb_screen_iterator_t sit = xcb_setup_roots_iterator(xcb_get_setup(conn));
    for (int i = 0; i < screen_num; i++) xcb_screen_next(&sit);
    xcb_screen_t *scr = sit.data;
    xcb_window_t root = scr->root;

    xcb_randr_query_version_reply_t *ver =
        xcb_randr_query_version_reply(conn,
            xcb_randr_query_version(conn, 1, 5), NULL);
    if (!ver) {
        fprintf(stderr, "isde-displayd: RandR not available\n");
        xcb_disconnect(conn);
        return 1;
    }
    free(ver);

    const xcb_query_extension_reply_t *ext =
        xcb_get_extension_data(conn, &xcb_randr_id);
    if (!ext || !ext->present) {
        fprintf(stderr, "isde-displayd: RandR extension not present\n");
        xcb_disconnect(conn);
        return 1;
    }
    uint8_t randr_event_base = ext->first_event;

    xcb_randr_select_input(conn, root,
        XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE |
        XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
    xcb_flush(conn);

    IsdeDBus *dbus = isde_dbus_init();
    if (dbus)
        isde_dbus_settings_subscribe(dbus, on_settings_changed, NULL);
    int dbus_fd = dbus ? isde_dbus_get_fd(dbus) : -1;

    struct udev *udev = udev_new();
    struct udev_monitor *udev_mon = NULL;
    int udev_fd = -1;
    if (udev) {
        udev_mon = udev_monitor_new_from_netlink(udev, "udev");
        if (udev_mon) {
            udev_monitor_filter_add_match_subsystem_devtype(
                udev_mon, "drm", NULL);
            udev_monitor_enable_receiving(udev_mon);
            udev_fd = udev_monitor_get_fd(udev_mon);
        }
    }

    apply_config(conn, root, scr);

    fprintf(stderr, "isde-displayd: watching for display changes\n");

    int xcb_fd = xcb_get_file_descriptor(conn);

    while (!quit_flag) {
        struct pollfd pfds[4];
        int nfds = 0;
        pfds[nfds++] = (struct pollfd){ .fd = xcb_fd, .events = POLLIN };
        if (dbus_fd >= 0)
            pfds[nfds++] = (struct pollfd){ .fd = dbus_fd, .events = POLLIN };
        if (udev_fd >= 0)
            pfds[nfds++] = (struct pollfd){ .fd = udev_fd, .events = POLLIN };

        poll(pfds, nfds, -1);

        if (dbus)
            isde_dbus_dispatch(dbus);

        if (reload_flag) {
            reload_flag = 0;
            fprintf(stderr, "isde-displayd: SIGHUP, reloading config\n");
            apply_config(conn, root, scr);
        }

        if (dbus_reload_flag) {
            dbus_reload_flag = 0;
            fprintf(stderr, "isde-displayd: settings changed, applying\n");
            apply_config(conn, root, scr);
        }

        int got_hotplug = 0;
        if (udev_mon) {
            struct udev_device *dev;
            while ((dev = udev_monitor_receive_device(udev_mon))) {
                got_hotplug = 1;
                udev_device_unref(dev);
            }
        }

        int got_randr = 0;
        xcb_generic_event_t *ev;
        while ((ev = xcb_poll_for_event(conn))) {
            uint8_t type = ev->response_type & ~0x80;
            if (type == randr_event_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY ||
                type == randr_event_base + XCB_RANDR_NOTIFY)
                got_randr = 1;
            free(ev);
        }

        if (xcb_connection_has_error(conn)) {
            fprintf(stderr, "isde-displayd: X connection lost\n");
            break;
        }

        if (got_randr || got_hotplug) {
            disable_disconnected(conn, root);

            usleep(200000);

            while ((ev = xcb_poll_for_event(conn)))
                free(ev);
            if (udev_mon) {
                struct udev_device *dev;
                while ((dev = udev_monitor_receive_device(udev_mon)))
                    udev_device_unref(dev);
            }

            enable_connected(conn, root, scr);
            xcb_dpms_force_level(conn, XCB_DPMS_DPMS_MODE_ON);
            xcb_force_screen_saver(conn, XCB_SCREEN_SAVER_RESET);
            xcb_flush(conn);
        }
    }

    if (udev_mon) udev_monitor_unref(udev_mon);
    if (udev) udev_unref(udev);
    isde_dbus_free(dbus);
    xcb_disconnect(conn);
    return 0;
}
