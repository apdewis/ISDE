#define _POSIX_C_SOURCE 200809L
/*
 * isde-displayd — display auto-configuration daemon
 *
 * Three distinct operations:
 *   disable_disconnected() — release CRTCs for physically absent outputs
 *   enable_connected()     — observe/plan/apply: reconfigure any connected
 *                            output whose current state doesn't match the
 *                            desired mode/geometry (handles reconnect races,
 *                            stale CRTCs, and transient set_crtc_config
 *                            failures via a single retry)
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

static int hash_cmp(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

/* Build a profile key from the EDID hashes of all connected outputs.
 * The key is a sorted, comma-separated list of hex hashes.
 * Caller must free() the returned string. Returns NULL on failure. */
static char *build_profile_key(xcb_connection_t *conn, xcb_window_t root)
{
    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, root), NULL);
    if (!res) {
        fprintf(stdout, "isde-displayd: build_profile_key: "
                "get_screen_resources failed\n");
        return NULL;
    }

    xcb_timestamp_t cfg_ts = res->config_timestamp;
    xcb_randr_output_t *outs =
        xcb_randr_get_screen_resources_current_outputs(res);
    int nouts = xcb_randr_get_screen_resources_current_outputs_length(res);

    fprintf(stdout, "isde-displayd: build_profile_key: %d outputs\n", nouts);

    char **hashes = malloc(nouts * sizeof(char *));
    int nhashes = 0;

    for (int i = 0; i < nouts; i++) {
        xcb_randr_get_output_info_reply_t *oi =
            xcb_randr_get_output_info_reply(conn,
                xcb_randr_get_output_info(conn, outs[i], cfg_ts), NULL);
        if (!oi) continue;

        int namelen = xcb_randr_get_output_info_name_length(oi);
        char oname[64];
        int cplen = namelen < 63 ? namelen : 63;
        memcpy(oname, xcb_randr_get_output_info_name(oi), cplen);
        oname[cplen] = '\0';

        if (oi->connection != XCB_RANDR_CONNECTION_CONNECTED) {
            fprintf(stdout, "isde-displayd: build_profile_key: "
                    "%s not connected (status %d)\n",
                    oname, oi->connection);
            free(oi);
            continue;
        }
        char *hash = isde_randr_read_edid_hash(conn, outs[i]);
        if (hash) {
            fprintf(stdout, "isde-displayd: build_profile_key: "
                    "%s edid=%s\n", oname, hash);
            hashes[nhashes++] = hash;
        } else {
            fprintf(stdout, "isde-displayd: build_profile_key: "
                    "%s connected but no EDID\n", oname);
        }
        free(oi);
    }
    free(res);

    if (nhashes == 0) {
        fprintf(stdout, "isde-displayd: build_profile_key: "
                "no EDID hashes, returning NULL\n");
        free(hashes);
        return NULL;
    }

    qsort(hashes, nhashes, sizeof(char *), hash_cmp);

    size_t keylen = 0;
    for (int i = 0; i < nhashes; i++)
        keylen += strlen(hashes[i]) + 1;

    char *key = malloc(keylen);
    key[0] = '\0';
    for (int i = 0; i < nhashes; i++) {
        if (i > 0) strcat(key, ",");
        strcat(key, hashes[i]);
        free(hashes[i]);
    }
    free(hashes);
    return key;
}

/* ---------- disable_disconnected ----------
 * Release CRTCs for outputs that are no longer physically present. */

static void disable_disconnected(xcb_connection_t *conn, xcb_window_t root)
{
    fprintf(stdout, "isde-displayd: disable_disconnected: enter\n");

    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, root), NULL);
    if (!res) {
        fprintf(stdout, "isde-displayd: disable_disconnected: "
                "get_screen_resources failed\n");
        return;
    }

    xcb_timestamp_t cfg_ts = res->config_timestamp;
    xcb_randr_output_t *outs =
        xcb_randr_get_screen_resources_current_outputs(res);
    int nouts = xcb_randr_get_screen_resources_current_outputs_length(res);

    for (int i = 0; i < nouts; i++) {
        xcb_randr_get_output_info_reply_t *oi =
            xcb_randr_get_output_info_reply(conn,
                xcb_randr_get_output_info(conn, outs[i], cfg_ts), NULL);
        if (!oi) continue;

        int namelen = xcb_randr_get_output_info_name_length(oi);
        char *name = strndup((char *)xcb_randr_get_output_info_name(oi),
                             namelen);

        if (oi->connection == XCB_RANDR_CONNECTION_CONNECTED) {
            fprintf(stdout, "isde-displayd: disable_disconnected: "
                    "%s still connected (crtc=%u), skipping\n",
                    name, (unsigned)oi->crtc);
            free(name);
            free(oi);
            continue;
        }
        if (oi->crtc == XCB_NONE) {
            fprintf(stdout, "isde-displayd: disable_disconnected: "
                    "%s disconnected, no CRTC assigned\n", name);
            free(name);
            free(oi);
            continue;
        }

        xcb_randr_set_crtc_config(conn, oi->crtc,
            XCB_CURRENT_TIME, cfg_ts,
            0, 0, XCB_NONE,
            XCB_RANDR_ROTATION_ROTATE_0, 0, NULL);
        fprintf(stdout, "isde-displayd: released CRTC %u for "
                "disconnected %s\n", (unsigned)oi->crtc, name);
        free(name);
        free(oi);
    }

    xcb_flush(conn);
    free(res);
}

/* ---------- enable_connected (refactored) ----------
 * Observe → Plan → Apply, so that a single monitor's
 * disconnect/reconnect cycle is handled robustly:
 *
 *   - Current CRTC state is compared against the desired
 *     configuration before any write, so an output the driver
 *     auto-enabled at the wrong mode is reconfigured instead of
 *     blindly skipped.
 *   - cfg_ts is refreshed once before the apply phase and again
 *     on retry, rather than only after a screen resize.
 *   - set_crtc_config failures (stale timestamp / CRTC
 *     contention) trigger a single retry with a fresh timestamp
 *     and fresh CRTC allocation.
 */

typedef enum {
    PLAN_SKIP,   /* not connected / disabled / no usable mode */
    PLAN_KEEP,   /* already at desired state, no write needed  */
    PLAN_APPLY   /* needs set_crtc_config                       */
} plan_action_t;

typedef struct {
    xcb_randr_output_t  id;
    char                name[64];
    int                 connected;
    xcb_randr_crtc_t    bound_crtc;
    int                 crtc_active;
    xcb_randr_mode_t    active_mode;
    int16_t             active_x, active_y;
    uint16_t            active_w, active_h;
    xcb_randr_get_output_info_reply_t *oi;
} output_state_t;

typedef struct {
    plan_action_t    action;
    xcb_randr_mode_t mode;
    int16_t          x, y;
    uint16_t         w, h;
    double           refresh;
    int              is_primary;
} desired_state_t;

static xcb_timestamp_t refresh_cfg_ts(xcb_connection_t *conn,
                                      xcb_window_t root,
                                      xcb_timestamp_t fallback)
{
    xcb_randr_get_screen_resources_current_reply_t *fresh =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, root), NULL);
    xcb_timestamp_t ts = fallback;
    if (fresh) {
        ts = fresh->config_timestamp;
        free(fresh);
    }
    return ts;
}

static void output_get_state(xcb_connection_t *conn, xcb_randr_output_t id,
                             xcb_timestamp_t cfg_ts, output_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->id = id;
    st->bound_crtc = XCB_NONE;
    st->active_mode = XCB_NONE;

    xcb_randr_get_output_info_reply_t *oi =
        xcb_randr_get_output_info_reply(conn,
            xcb_randr_get_output_info(conn, id, cfg_ts), NULL);
    if (!oi) return;
    st->oi = oi;
    st->connected = (oi->connection == XCB_RANDR_CONNECTION_CONNECTED);

    int namelen = xcb_randr_get_output_info_name_length(oi);
    int cplen = namelen < 63 ? namelen : 63;
    memcpy(st->name, xcb_randr_get_output_info_name(oi), cplen);
    st->name[cplen] = '\0';

    st->bound_crtc = oi->crtc;

    if (oi->crtc != XCB_NONE) {
        xcb_randr_get_crtc_info_reply_t *ci =
            xcb_randr_get_crtc_info_reply(conn,
                xcb_randr_get_crtc_info(conn, oi->crtc, cfg_ts), NULL);
        if (ci) {
            st->crtc_active = (ci->mode != XCB_NONE);
            st->active_mode = ci->mode;
            st->active_x = ci->x;
            st->active_y = ci->y;
            st->active_w = ci->width;
            st->active_h = ci->height;
            free(ci);
        }
    }
}

static xcb_randr_crtc_t acquire_crtc(xcb_connection_t *conn,
                                     const output_state_t *st,
                                     xcb_timestamp_t cfg_ts)
{
    if (st->bound_crtc != XCB_NONE)
        return st->bound_crtc;
    return isde_randr_find_free_crtc(conn, st->oi, cfg_ts);
}

static void plan_output(const output_state_t *st,
                        IsdeConfigTable *outs_tbl,
                        xcb_randr_mode_info_t *mode_infos,
                        int nmi, desired_state_t *out)
{
    memset(out, 0, sizeof(*out));
    out->action = PLAN_SKIP;
    out->mode = XCB_NONE;

    if (!st->connected) return;

    IsdeConfigTable *mon = outs_tbl ?
        isde_config_table(outs_tbl, st->name) : NULL;

    if (mon && !isde_config_bool(mon, "enabled", 1)) return;

    int want_w = mon ? (int)isde_config_int(mon, "width", 0) : 0;
    int want_h = mon ? (int)isde_config_int(mon, "height", 0) : 0;
    int want_x = mon ? (int)isde_config_int(mon, "x", 0) : 0;
    int want_y = mon ? (int)isde_config_int(mon, "y", 0) : 0;
    out->is_primary = mon ? isde_config_bool(mon, "primary", 0) : 0;

    if (want_w <= 0 || want_h <= 0) {
        xcb_randr_mode_t *pref_modes =
            xcb_randr_get_output_info_modes(st->oi);
        if (xcb_randr_get_output_info_modes_length(st->oi) > 0 &&
            st->oi->num_preferred > 0) {
            for (int k = 0; k < nmi; k++) {
                if (mode_infos[k].id == pref_modes[0]) {
                    want_w = mode_infos[k].width;
                    want_h = mode_infos[k].height;
                    break;
                }
            }
        }
    }

    if (want_w <= 0 || want_h <= 0) return;

    out->w = (uint16_t)want_w;
    out->h = (uint16_t)want_h;
    out->x = (int16_t)want_x;
    out->y = (int16_t)want_y;

    xcb_randr_mode_t *out_modes =
        xcb_randr_get_output_info_modes(st->oi);
    int out_nmodes = xcb_randr_get_output_info_modes_length(st->oi);
    for (int m = 0; m < out_nmodes; m++) {
        for (int k = 0; k < nmi; k++) {
            if (mode_infos[k].id != out_modes[m]) continue;
            if (mode_infos[k].width == out->w &&
                mode_infos[k].height == out->h) {
                double r = isde_randr_refresh(&mode_infos[k]);
                if (r > out->refresh) {
                    out->refresh = r;
                    out->mode = mode_infos[k].id;
                }
            }
        }
    }

    if (out->mode == XCB_NONE) return;

    if (st->crtc_active && st->active_mode == out->mode &&
        st->active_x == out->x && st->active_y == out->y &&
        st->active_w == out->w && st->active_h == out->h) {
        out->action = PLAN_KEEP;
        return;
    }

    out->action = PLAN_APPLY;
}

static int apply_desired(xcb_connection_t *conn, xcb_window_t root,
                         const output_state_t *st,
                         const desired_state_t *des, xcb_timestamp_t *cfg_ts)
{
    xcb_randr_crtc_t crtc = acquire_crtc(conn, st, *cfg_ts);
    if (crtc == XCB_NONE) {
        fprintf(stdout, "isde-displayd: no free CRTC for %s\n", st->name);
        return 0;
    }

    *cfg_ts = refresh_cfg_ts(conn, root, *cfg_ts);

    xcb_randr_output_t out_id = st->id;
    xcb_randr_set_crtc_config_cookie_t ck =
        xcb_randr_set_crtc_config(conn, crtc,
            XCB_CURRENT_TIME, *cfg_ts,
            des->x, des->y, des->mode,
            XCB_RANDR_ROTATION_ROTATE_0,
            1, &out_id);
    xcb_randr_set_crtc_config_reply_t *cr =
        xcb_randr_set_crtc_config_reply(conn, ck, NULL);

    if (!cr || cr->status != XCB_RANDR_SET_CONFIG_SUCCESS) {
        fprintf(stdout, "isde-displayd: set_crtc_config failed for %s"
                " (status %d), retrying\n",
                st->name, cr ? cr->status : -1);
        free(cr);

        *cfg_ts = refresh_cfg_ts(conn, root, *cfg_ts);
        xcb_randr_crtc_t crtc2 =
            isde_randr_find_free_crtc(conn, st->oi, *cfg_ts);
        if (crtc2 != XCB_NONE)
            crtc = crtc2;

        out_id = st->id;
        ck = xcb_randr_set_crtc_config(conn, crtc,
            XCB_CURRENT_TIME, *cfg_ts,
            des->x, des->y, des->mode,
            XCB_RANDR_ROTATION_ROTATE_0,
            1, &out_id);
        cr = xcb_randr_set_crtc_config_reply(conn, ck, NULL);
        if (!cr || cr->status != XCB_RANDR_SET_CONFIG_SUCCESS) {
            fprintf(stdout, "isde-displayd: set_crtc_config retry failed"
                    " for %s (status %d)\n",
                    st->name, cr ? cr->status : -1);
            free(cr);
            return 0;
        }
    }

    fprintf(stdout, "isde-displayd: enabled %s -> %ux%u+%d+%d"
            " @ %.0f Hz (crtc %u)\n",
            st->name, des->w, des->h, des->x, des->y, des->refresh,
            (unsigned)crtc);
    free(cr);
    return 1;
}

static int enable_connected(xcb_connection_t *conn, xcb_window_t root,
                             xcb_screen_t *scr)
{
    fprintf(stdout, "isde-displayd: enable_connected: enter\n");

    xcb_randr_get_screen_resources_reply_t *res =
        xcb_randr_get_screen_resources_reply(conn,
            xcb_randr_get_screen_resources(conn, root), NULL);
    if (!res) {
        fprintf(stdout, "isde-displayd: enable_connected: "
                "get_screen_resources failed\n");
        return 0;
    }

    xcb_randr_mode_info_t *mode_infos =
        xcb_randr_get_screen_resources_modes(res);
    int nmi = xcb_randr_get_screen_resources_modes_length(res);
    xcb_timestamp_t cfg_ts = res->config_timestamp;
    xcb_randr_output_t *outs =
        xcb_randr_get_screen_resources_outputs(res);
    int nouts = xcb_randr_get_screen_resources_outputs_length(res);

    fprintf(stdout, "isde-displayd: enable_connected: "
            "%d outputs, %d modes\n", nouts, nmi);

    char *profile_key = build_profile_key(conn, root);

    IsdeConfigTable *outs_tbl = NULL;
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (cfg && profile_key) {
        IsdeConfigTable *cfg_root = isde_config_root(cfg);
        IsdeConfigTable *disp = isde_config_table(cfg_root, "display");
        IsdeConfigTable *profiles = disp ?
            isde_config_table(disp, "profiles") : NULL;
        IsdeConfigTable *profile = profiles ?
            isde_config_table(profiles, profile_key) : NULL;
        if (profile)
            outs_tbl = isde_config_table(profile, "outputs");
    }

    fprintf(stdout, "isde-displayd: enable_connected: profile=%s outs_tbl=%s\n",
            profile_key ? profile_key : "(null)",
            outs_tbl ? "found" : "none");

    if (nouts == 0) {
        free(res); free(profile_key); isde_config_free(cfg);
        return 0;
    }

    output_state_t  *states = calloc(nouts, sizeof(output_state_t));
    desired_state_t *plans  = calloc(nouts, sizeof(desired_state_t));
    if (!states || !plans) {
        free(states); free(plans);
        free(res); free(profile_key); isde_config_free(cfg);
        return 0;
    }

    /* ---- Phase A: observe ---- */
    for (int i = 0; i < nouts; i++) {
        output_get_state(conn, outs[i], cfg_ts, &states[i]);
        fprintf(stdout, "isde-displayd: enable_connected: "
                "%s: connected=%d crtc=%u active=%d mode=%u "
                "%ux%u+%d+%d\n",
                states[i].name, states[i].connected,
                (unsigned)states[i].bound_crtc, states[i].crtc_active,
                (unsigned)states[i].active_mode,
                states[i].active_w, states[i].active_h,
                states[i].active_x, states[i].active_y);
    }

    /* ---- Phase B: plan ---- */
    int n_to_apply = 0;
    for (int i = 0; i < nouts; i++) {
        plan_output(&states[i], outs_tbl, mode_infos, nmi, &plans[i]);
        const char *act = plans[i].action == PLAN_KEEP ? "keep" :
                          plans[i].action == PLAN_APPLY ? "apply" : "skip";
        fprintf(stdout, "isde-displayd: enable_connected: %s: plan=%s\n",
                states[i].name, act);
        if (plans[i].action == PLAN_APPLY)
            n_to_apply++;
    }

    /* ---- Pre-compute screen resize from all planned geometries ---- */
    if (n_to_apply) {
        xcb_get_geometry_reply_t *rg =
            xcb_get_geometry_reply(conn, xcb_get_geometry(conn, root), NULL);
        uint16_t cur_sw = rg ? rg->width  : scr->width_in_pixels;
        uint16_t cur_sh = rg ? rg->height : scr->height_in_pixels;
        free(rg);

        int need_w = cur_sw, need_h = cur_sh;
        for (int i = 0; i < nouts; i++) {
            int right, bottom;
            if (plans[i].action == PLAN_APPLY) {
                right  = plans[i].x + plans[i].w;
                bottom = plans[i].y + plans[i].h;
            } else if (plans[i].action == PLAN_KEEP) {
                right  = states[i].active_x + states[i].active_w;
                bottom = states[i].active_y + states[i].active_h;
            } else {
                continue;
            }
            if (right  > need_w) need_w = right;
            if (bottom > need_h) need_h = bottom;
        }

        if (need_w > (int)cur_sw || need_h > (int)cur_sh) {
            int mm_w = scr->width_in_pixels > 0 ?
                (need_w * scr->width_in_millimeters) /
                scr->width_in_pixels : need_w;
            int mm_h = scr->height_in_pixels > 0 ?
                (need_h * scr->height_in_millimeters) /
                scr->height_in_pixels : need_h;
            xcb_randr_set_screen_size(conn, root, need_w, need_h, mm_w, mm_h);
        }
    }

    /* ---- Phase C: apply ---- */
    xcb_randr_output_t first_enabled = XCB_NONE;

    for (int i = 0; i < nouts; i++) {
        if (plans[i].action == PLAN_KEEP) {
            if (first_enabled == XCB_NONE)
                first_enabled = outs[i];
            continue;
        }
        if (plans[i].action != PLAN_APPLY)
            continue;

        int ok = apply_desired(conn, root,
                                &states[i], &plans[i], &cfg_ts);
        if (ok && first_enabled == XCB_NONE)
            first_enabled = outs[i];
    }

    if (first_enabled != XCB_NONE)
        xcb_randr_set_output_primary(conn, root, first_enabled);

    fprintf(stdout, "isde-displayd: enable_connected: done, "
            "first_enabled=%s\n",
            first_enabled != XCB_NONE ? "yes" : "none");

    for (int i = 0; i < nouts; i++)
        free(states[i].oi);
    free(states);
    free(plans);
    xcb_flush(conn);
    free(res);
    free(profile_key);
    isde_config_free(cfg);
    return first_enabled != XCB_NONE;
}

/* ---------- apply_config ----------
 * Full config application from isde.toml: look up the profile matching
 * the current connected monitor combination.  Disable outputs marked
 * off, set modes/positions for all enabled outputs.  When no profile
 * exists, enable all connected outputs at preferred modes with the
 * first as primary.  Used on startup, SIGHUP, and settings-panel
 * changes.  Shares observe/plan/apply helpers with enable_connected. */

static int apply_config(xcb_connection_t *conn, xcb_window_t root,
                         xcb_screen_t *scr)
{
    fprintf(stdout, "isde-displayd: apply_config: enter\n");

    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (!cfg) {
        fprintf(stdout, "isde-displayd: apply_config: "
                "failed to load isde.toml: %s\n", errbuf);
        return 0;
    }

    char *profile_key = build_profile_key(conn, root);
    fprintf(stdout, "isde-displayd: apply_config: profile_key=%s\n",
            profile_key ? profile_key : "(null)");

    IsdeConfigTable *cfg_root = isde_config_root(cfg);
    IsdeConfigTable *disp = isde_config_table(cfg_root, "display");
    if (!disp) {
        fprintf(stdout, "isde-displayd: apply_config: "
                "no [display] section in config\n");
        free(profile_key);
        isde_config_free(cfg);
        return 0;
    }

    IsdeConfigTable *outs_tbl = NULL;
    if (profile_key) {
        IsdeConfigTable *profiles = isde_config_table(disp, "profiles");
        IsdeConfigTable *profile = profiles ?
            isde_config_table(profiles, profile_key) : NULL;
        if (profile)
            outs_tbl = isde_config_table(profile, "outputs");
        fprintf(stdout, "isde-displayd: apply_config: "
                "profiles=%s profile=%s outs_tbl=%s\n",
                profiles ? "found" : "none",
                profile ? "found" : "none",
                outs_tbl ? "found" : "none");
    }

    if (!outs_tbl) {
        fprintf(stdout, "isde-displayd: apply_config: no profile for "
                "current monitors%s%s%s, falling back to enable_connected\n",
                profile_key ? " (" : "",
                profile_key ? profile_key : "",
                profile_key ? ")" : "");
        free(profile_key);
        isde_config_free(cfg);
        return enable_connected(conn, root, scr);
    }

    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, root), NULL);
    if (!res) {
        fprintf(stdout, "isde-displayd: apply_config: "
                "get_screen_resources failed\n");
        free(profile_key);
        isde_config_free(cfg);
        return 0;
    }

    xcb_randr_mode_info_t *mode_infos =
        xcb_randr_get_screen_resources_current_modes(res);
    int nmi = xcb_randr_get_screen_resources_current_modes_length(res);
    xcb_timestamp_t cfg_ts = res->config_timestamp;

    xcb_randr_output_t *randr_outs =
        xcb_randr_get_screen_resources_current_outputs(res);
    int n_randr_outs =
        xcb_randr_get_screen_resources_current_outputs_length(res);

    /* ---- Phase 0: disable outputs that config says should be off ---- */
    int changed = 0;
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
            fprintf(stdout, "isde-displayd: disabled %s\n", name);
            changed = 1;
        }

        free(name);
        free(oinfo);
    }

    if (changed) {
        free(res);
        res = xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, root), NULL);
        if (!res) { free(profile_key); isde_config_free(cfg); return 0; }
        mode_infos = xcb_randr_get_screen_resources_current_modes(res);
        nmi = xcb_randr_get_screen_resources_current_modes_length(res);
        cfg_ts = res->config_timestamp;
        randr_outs = xcb_randr_get_screen_resources_current_outputs(res);
        n_randr_outs =
            xcb_randr_get_screen_resources_current_outputs_length(res);
    }

    if (n_randr_outs == 0) {
        free(res); free(profile_key); isde_config_free(cfg);
        return 0;
    }

    output_state_t  *states = calloc(n_randr_outs, sizeof(output_state_t));
    desired_state_t *plans  = calloc(n_randr_outs, sizeof(desired_state_t));
    if (!states || !plans) {
        free(states); free(plans);
        free(res); free(profile_key); isde_config_free(cfg);
        return 0;
    }

    /* ---- Phase A: observe ---- */
    for (int i = 0; i < n_randr_outs; i++) {
        output_get_state(conn, randr_outs[i], cfg_ts, &states[i]);
        fprintf(stdout, "isde-displayd: apply_config: "
                "%s: connected=%d crtc=%u active=%d mode=%u "
                "%ux%u+%d+%d\n",
                states[i].name, states[i].connected,
                (unsigned)states[i].bound_crtc, states[i].crtc_active,
                (unsigned)states[i].active_mode,
                states[i].active_w, states[i].active_h,
                states[i].active_x, states[i].active_y);
    }

    /* ---- Phase B: plan ---- */
    int n_to_apply = 0;
    for (int i = 0; i < n_randr_outs; i++) {
        plan_output(&states[i], outs_tbl, mode_infos, nmi, &plans[i]);
        const char *act = plans[i].action == PLAN_KEEP ? "keep" :
                          plans[i].action == PLAN_APPLY ? "apply" : "skip";
        fprintf(stdout, "isde-displayd: apply_config: %s: plan=%s"
                "%s%s\n",
                states[i].name, act,
                plans[i].action == PLAN_APPLY ? " " : "",
                plans[i].action == PLAN_APPLY && plans[i].is_primary ?
                    "(primary)" :
                plans[i].action == PLAN_APPLY ? "" : "");
        if (plans[i].action == PLAN_APPLY)
            n_to_apply++;
    }

    /* ---- Pre-compute screen resize from all planned geometries ---- */
    if (n_to_apply) {
        xcb_get_geometry_reply_t *rg =
            xcb_get_geometry_reply(conn, xcb_get_geometry(conn, root), NULL);
        uint16_t cur_sw = rg ? rg->width  : scr->width_in_pixels;
        uint16_t cur_sh = rg ? rg->height : scr->height_in_pixels;
        free(rg);

        int need_w = cur_sw, need_h = cur_sh;
        for (int i = 0; i < n_randr_outs; i++) {
            int right, bottom;
            if (plans[i].action == PLAN_APPLY) {
                right  = plans[i].x + plans[i].w;
                bottom = plans[i].y + plans[i].h;
            } else if (plans[i].action == PLAN_KEEP) {
                right  = states[i].active_x + states[i].active_w;
                bottom = states[i].active_y + states[i].active_h;
            } else {
                continue;
            }
            if (right  > need_w) need_w = right;
            if (bottom > need_h) need_h = bottom;
        }

        if (need_w > (int)cur_sw || need_h > (int)cur_sh) {
            int mm_w = scr->width_in_pixels > 0 ?
                (need_w * scr->width_in_millimeters) /
                scr->width_in_pixels : need_w;
            int mm_h = scr->height_in_pixels > 0 ?
                (need_h * scr->height_in_millimeters) /
                scr->height_in_pixels : need_h;
            xcb_randr_set_screen_size(conn, root, need_w, need_h, mm_w, mm_h);
        }
    }

    /* ---- Phase C: apply ---- */
    xcb_randr_output_t primary_out = XCB_NONE;
    xcb_randr_output_t first_enabled = XCB_NONE;

    for (int i = 0; i < n_randr_outs; i++) {
        if (plans[i].action == PLAN_KEEP) {
            if (first_enabled == XCB_NONE)
                first_enabled = randr_outs[i];
            if (plans[i].is_primary)
                primary_out = randr_outs[i];
            continue;
        }
        if (plans[i].action != PLAN_APPLY)
            continue;

        int ok = apply_desired(conn, root,
                                &states[i], &plans[i], &cfg_ts);
        if (ok) {
            if (first_enabled == XCB_NONE)
                first_enabled = randr_outs[i];
            if (plans[i].is_primary)
                primary_out = randr_outs[i];
        }
    }

    xcb_randr_output_t new_primary =
        primary_out != XCB_NONE ? primary_out : first_enabled;
    if (new_primary != XCB_NONE)
        xcb_randr_set_output_primary(conn, root, new_primary);

    fprintf(stdout, "isde-displayd: apply_config: done, "
            "first_enabled=%s\n",
            first_enabled != XCB_NONE ? "yes" : "none");

    for (int i = 0; i < n_randr_outs; i++)
        free(states[i].oi);
    free(states);
    free(plans);
    xcb_flush(conn);
    free(res);
    free(profile_key);
    isde_config_free(cfg);
    return first_enabled != XCB_NONE;
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

    setlinebuf(stdout);

    const char *display = getenv("DISPLAY");
    int screen_num;
    xcb_connection_t *conn = xcb_connect(display, &screen_num);
    if (xcb_connection_has_error(conn)) {
        fprintf(stdout, "isde-displayd: cannot connect to X\n");
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
        fprintf(stdout, "isde-displayd: RandR not available\n");
        xcb_disconnect(conn);
        return 1;
    }
    free(ver);

    const xcb_query_extension_reply_t *ext =
        xcb_get_extension_data(conn, &xcb_randr_id);
    if (!ext || !ext->present) {
        fprintf(stdout, "isde-displayd: RandR extension not present\n");
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

    fprintf(stdout, "isde-displayd: watching for display changes\n");

    int xcb_fd = xcb_get_file_descriptor(conn);
    int no_outputs = 0;

    while (!quit_flag) {
        struct pollfd pfds[4];
        int nfds = 0;
        pfds[nfds++] = (struct pollfd){ .fd = xcb_fd, .events = POLLIN };
        if (dbus_fd >= 0)
            pfds[nfds++] = (struct pollfd){ .fd = dbus_fd, .events = POLLIN };
        if (udev_fd >= 0)
            pfds[nfds++] = (struct pollfd){ .fd = udev_fd, .events = POLLIN };

        int timeout = no_outputs ? 1000 : -1;
        poll(pfds, nfds, timeout);

        if (dbus)
            isde_dbus_dispatch(dbus);

        if (reload_flag) {
            reload_flag = 0;
            fprintf(stdout, "isde-displayd: SIGHUP, reloading config\n");
            apply_config(conn, root, scr);
        }

        if (dbus_reload_flag) {
            dbus_reload_flag = 0;
            fprintf(stdout, "isde-displayd: settings changed, applying\n");
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
            fprintf(stdout, "isde-displayd: X connection lost\n");
            break;
        }

        if (got_randr || got_hotplug) {
            fprintf(stdout, "isde-displayd: hotplug event "
                    "(randr=%d udev=%d)\n", got_randr, got_hotplug);

            disable_disconnected(conn, root);

            fprintf(stdout, "isde-displayd: waiting 200ms for "
                    "hardware settle\n");
            usleep(200000);

            int drained_xcb = 0, drained_udev = 0;
            while ((ev = xcb_poll_for_event(conn))) {
                drained_xcb++;
                free(ev);
            }
            if (udev_mon) {
                struct udev_device *dev;
                while ((dev = udev_monitor_receive_device(udev_mon))) {
                    drained_udev++;
                    udev_device_unref(dev);
                }
            }
            if (drained_xcb || drained_udev) {
                fprintf(stdout, "isde-displayd: drained %d xcb + "
                        "%d udev events during settle\n",
                        drained_xcb, drained_udev);
            }

            no_outputs = !apply_config(conn, root, scr);
            if (no_outputs) {
                fprintf(stdout, "isde-displayd: no outputs configured, "
                        "will poll every 1s\n");
            }
            xcb_dpms_force_level(conn, XCB_DPMS_DPMS_MODE_ON);
            xcb_force_screen_saver(conn, XCB_SCREEN_SAVER_RESET);
            xcb_flush(conn);
            fprintf(stdout, "isde-displayd: hotplug handling complete\n");
        }

        if (no_outputs && !got_randr && !got_hotplug) {
            xcb_randr_get_screen_resources_reply_t *probe =
                xcb_randr_get_screen_resources_reply(conn,
                    xcb_randr_get_screen_resources(conn, root), NULL);
            if (probe) {
                xcb_timestamp_t ts = probe->config_timestamp;
                xcb_randr_output_t *pouts =
                    xcb_randr_get_screen_resources_outputs(probe);
                int npouts =
                    xcb_randr_get_screen_resources_outputs_length(probe);
                int found = 0;
                for (int j = 0; j < npouts; j++) {
                    xcb_randr_get_output_info_reply_t *oi =
                        xcb_randr_get_output_info_reply(conn,
                            xcb_randr_get_output_info(conn, pouts[j],
                                                      ts), NULL);
                    if (oi) {
                        if (oi->connection == XCB_RANDR_CONNECTION_CONNECTED)
                            found = 1;
                        free(oi);
                    }
                    if (found) break;
                }
                free(probe);

                if (found) {
                    fprintf(stdout, "isde-displayd: output detected "
                            "on poll probe, configuring\n");
                    no_outputs = !apply_config(conn, root, scr);
                    if (!no_outputs) {
                        xcb_dpms_force_level(conn, XCB_DPMS_DPMS_MODE_ON);
                        xcb_force_screen_saver(conn, XCB_SCREEN_SAVER_RESET);
                        xcb_flush(conn);
                    }
                }
            }
        }
    }

    if (udev_mon) udev_monitor_unref(udev_mon);
    if (udev) udev_unref(udev);
    isde_dbus_free(dbus);
    xcb_disconnect(conn);
    return 0;
}
