#define _POSIX_C_SOURCE 200809L
/*
 * isde-display-x11.c — X11 backend: monitor query (+ config stub) ops.
 *
 * Absorbs the body of the former isde-randr.c, reshaped onto IsdeDisplay.
 */
#include "isde-platform-x11.h"

#include <stdlib.h>
#include <xcb/randr.h>

static double mode_refresh_info(const xcb_randr_mode_info_t *mi)
{
    if (mi->htotal == 0 || mi->vtotal == 0) {
        return 0.0;
    }
    double vt = mi->vtotal;
    if (mi->mode_flags & XCB_RANDR_MODE_FLAG_DOUBLE_SCAN) {
        vt *= 2;
    }
    if (mi->mode_flags & XCB_RANDR_MODE_FLAG_INTERLACE) {
        vt /= 2;
    }
    return (double)mi->dot_clock / ((double)mi->htotal * vt);
}

static int primary(IsdeDisplay *d, IsdeMonitor *out)
{
    xcb_connection_t *conn = d->conn;
    xcb_window_t root = d->screen->root;

    out->x = 0;
    out->y = 0;
    out->width  = d->screen->width_in_pixels;
    out->height = d->screen->height_in_pixels;

    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, root), NULL);
    if (!res) {
        return 0;
    }

    xcb_timestamp_t ts = res->config_timestamp;
    xcb_randr_get_output_primary_reply_t *pri =
        xcb_randr_get_output_primary_reply(conn,
            xcb_randr_get_output_primary(conn, root), NULL);
    xcb_randr_output_t primary_id = pri ? pri->output : XCB_NONE;
    free(pri);

    xcb_randr_output_t *outs =
        xcb_randr_get_screen_resources_current_outputs(res);
    int nouts = xcb_randr_get_screen_resources_current_outputs_length(res);

    xcb_randr_crtc_t fallback_crtc = XCB_NONE;

    for (int i = 0; i < nouts; i++) {
        xcb_randr_get_output_info_reply_t *oi =
            xcb_randr_get_output_info_reply(conn,
                xcb_randr_get_output_info(conn, outs[i], ts), NULL);
        if (!oi) {
            continue;
        }
        if (oi->connection != XCB_RANDR_CONNECTION_CONNECTED ||
            oi->crtc == XCB_NONE) {
            free(oi);
            continue;
        }
        if (fallback_crtc == XCB_NONE) {
            fallback_crtc = oi->crtc;
        }
        if (outs[i] == primary_id) {
            xcb_randr_get_crtc_info_reply_t *ci =
                xcb_randr_get_crtc_info_reply(conn,
                    xcb_randr_get_crtc_info(conn, oi->crtc, ts), NULL);
            if (ci) {
                out->x = ci->x;
                out->y = ci->y;
                out->width  = ci->width;
                out->height = ci->height;
                free(ci);
            }
            free(oi);
            free(res);
            return 1;
        }
        free(oi);
    }

    if (fallback_crtc != XCB_NONE) {
        xcb_randr_get_crtc_info_reply_t *ci =
            xcb_randr_get_crtc_info_reply(conn,
                xcb_randr_get_crtc_info(conn, fallback_crtc, ts), NULL);
        if (ci) {
            out->x = ci->x;
            out->y = ci->y;
            out->width  = ci->width;
            out->height = ci->height;
            free(ci);
            free(res);
            return 1;
        }
    }

    free(res);
    return 0;
}

static int monitor_at(IsdeDisplay *d, int px, int py, IsdeMonitor *out)
{
    xcb_connection_t *conn = d->conn;
    xcb_window_t root = d->screen->root;

    out->x = 0;
    out->y = 0;
    out->width  = d->screen->width_in_pixels;
    out->height = d->screen->height_in_pixels;

    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, root), NULL);
    if (!res) {
        return 0;
    }

    xcb_timestamp_t ts = res->config_timestamp;
    xcb_randr_crtc_t *crtcs =
        xcb_randr_get_screen_resources_current_crtcs(res);
    int ncrtcs = xcb_randr_get_screen_resources_current_crtcs_length(res);

    int found_any = 0;
    for (int i = 0; i < ncrtcs; i++) {
        xcb_randr_get_crtc_info_reply_t *ci =
            xcb_randr_get_crtc_info_reply(conn,
                xcb_randr_get_crtc_info(conn, crtcs[i], ts), NULL);
        if (!ci) {
            continue;
        }
        if (ci->mode == XCB_NONE || ci->num_outputs == 0) {
            free(ci);
            continue;
        }

        if (!found_any) {
            out->x = ci->x;
            out->y = ci->y;
            out->width  = ci->width;
            out->height = ci->height;
            found_any = 1;
        }

        if (px >= ci->x && px < ci->x + ci->width &&
            py >= ci->y && py < ci->y + ci->height) {
            out->x = ci->x;
            out->y = ci->y;
            out->width  = ci->width;
            out->height = ci->height;
            free(ci);
            free(res);
            return 1;
        }
        free(ci);
    }

    free(res);
    return found_any;
}

static int monitors(IsdeDisplay *d, IsdeMonitor **out)
{
    xcb_connection_t *conn = d->conn;
    xcb_window_t root = d->screen->root;

    *out = NULL;

    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, root), NULL);
    if (!res) {
        return 0;
    }

    xcb_timestamp_t ts = res->config_timestamp;
    xcb_randr_crtc_t *crtcs =
        xcb_randr_get_screen_resources_current_crtcs(res);
    int ncrtcs = xcb_randr_get_screen_resources_current_crtcs_length(res);

    *out = malloc(ncrtcs * sizeof(IsdeMonitor));
    int count = 0;

    for (int i = 0; i < ncrtcs; i++) {
        xcb_randr_get_crtc_info_reply_t *ci =
            xcb_randr_get_crtc_info_reply(conn,
                xcb_randr_get_crtc_info(conn, crtcs[i], ts), NULL);
        if (!ci) {
            continue;
        }
        if (ci->mode == XCB_NONE || ci->num_outputs == 0) {
            free(ci);
            continue;
        }

        /* Verify at least one output is connected */
        xcb_randr_output_t *crtc_outs =
            xcb_randr_get_crtc_info_outputs(ci);
        int n_crtc_outs = xcb_randr_get_crtc_info_outputs_length(ci);
        int has_connected = 0;
        for (int j = 0; j < n_crtc_outs; j++) {
            xcb_randr_get_output_info_reply_t *oi =
                xcb_randr_get_output_info_reply(conn,
                    xcb_randr_get_output_info(conn, crtc_outs[j], ts), NULL);
            if (oi) {
                if (oi->connection == XCB_RANDR_CONNECTION_CONNECTED) {
                    has_connected = 1;
                }
                free(oi);
                if (has_connected) {
                    break;
                }
            }
        }

        if (!has_connected) {
            free(ci);
            continue;
        }

        IsdeMonitor *m = &(*out)[count++];
        m->x = ci->x;
        m->y = ci->y;
        m->width  = ci->width;
        m->height = ci->height;
        free(ci);
    }

    free(res);
    return count;
}

const IsdePlatformDisplayOps isde_x11_display_ops = {
    .primary    = primary,
    .monitor_at = monitor_at,
    .monitors   = monitors,
};

/* ---------- config sub-vtable (settings + displayd; query parts now) ---------- */

static double mode_refresh(IsdeDisplay *d, uint32_t mode_id)
{
    xcb_connection_t *conn = d->conn;
    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, d->screen->root), NULL);
    if (!res) {
        return 0.0;
    }

    double hz = 0.0;
    xcb_randr_mode_info_t *modes =
        xcb_randr_get_screen_resources_current_modes(res);
    int nmodes = xcb_randr_get_screen_resources_current_modes_length(res);
    for (int i = 0; i < nmodes; i++) {
        if (modes[i].id == mode_id) {
            hz = mode_refresh_info(&modes[i]);
            break;
        }
    }
    free(res);
    return hz;
}

static int outputs(IsdeDisplay *d, uint32_t **ids)
{
    xcb_connection_t *conn = d->conn;
    *ids = NULL;
    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, d->screen->root), NULL);
    if (!res) {
        return 0;
    }
    xcb_randr_output_t *outs =
        xcb_randr_get_screen_resources_current_outputs(res);
    int n = xcb_randr_get_screen_resources_current_outputs_length(res);
    if (n > 0) {
        *ids = malloc(n * sizeof(uint32_t));
        if (*ids) {
            for (int i = 0; i < n; i++) {
                (*ids)[i] = outs[i];
            }
        } else {
            n = 0;
        }
    }
    free(res);
    return n;
}

const IsdePlatformDisplayConfigOps isde_x11_display_config_ops = {
    .mode_refresh = mode_refresh,
    .outputs      = outputs,
};
