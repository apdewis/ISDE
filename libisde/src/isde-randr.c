#define _POSIX_C_SOURCE 200809L
/*
 * isde-randr.c — shared RandR helpers
 */
#include "isde/isde-randr.h"
#include <stdlib.h>

double isde_randr_refresh(xcb_randr_mode_info_t *mi)
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

int isde_randr_primary(xcb_connection_t *conn, xcb_window_t root,
                       xcb_screen_t *scr, IsdeMonitor *out)
{
    out->x = 0;
    out->y = 0;
    out->width  = scr->width_in_pixels;
    out->height = scr->height_in_pixels;

    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, root), NULL);
    if (!res) return 0;

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
        if (!oi) continue;
        if (oi->connection != XCB_RANDR_CONNECTION_CONNECTED ||
            oi->crtc == XCB_NONE) {
            free(oi);
            continue;
        }
        if (fallback_crtc == XCB_NONE)
            fallback_crtc = oi->crtc;
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

int isde_randr_monitor_at(xcb_connection_t *conn, xcb_window_t root,
                          xcb_screen_t *scr, int px, int py,
                          IsdeMonitor *out)
{
    out->x = 0;
    out->y = 0;
    out->width  = scr->width_in_pixels;
    out->height = scr->height_in_pixels;

    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, root), NULL);
    if (!res) return 0;

    xcb_timestamp_t ts = res->config_timestamp;
    xcb_randr_crtc_t *crtcs =
        xcb_randr_get_screen_resources_current_crtcs(res);
    int ncrtcs = xcb_randr_get_screen_resources_current_crtcs_length(res);

    int found_any = 0;
    for (int i = 0; i < ncrtcs; i++) {
        xcb_randr_get_crtc_info_reply_t *ci =
            xcb_randr_get_crtc_info_reply(conn,
                xcb_randr_get_crtc_info(conn, crtcs[i], ts), NULL);
        if (!ci) continue;
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

xcb_randr_crtc_t isde_randr_find_free_crtc(xcb_connection_t *conn,
                                            xcb_randr_get_output_info_reply_t *oinfo,
                                            xcb_timestamp_t cfg_ts)
{
    xcb_randr_crtc_t *crtcs = xcb_randr_get_output_info_crtcs(oinfo);
    int ncrtcs = xcb_randr_get_output_info_crtcs_length(oinfo);

    for (int i = 0; i < ncrtcs; i++) {
        xcb_randr_get_crtc_info_reply_t *ci =
            xcb_randr_get_crtc_info_reply(conn,
                xcb_randr_get_crtc_info(conn, crtcs[i], cfg_ts), NULL);
        if (!ci) continue;
        int avail = (ci->num_outputs == 0 && ci->mode == XCB_NONE);
        free(ci);
        if (avail) return crtcs[i];
    }
    return XCB_NONE;
}

int isde_randr_monitors(xcb_connection_t *conn, xcb_window_t root,
                        IsdeMonitor **out)
{
    *out = NULL;

    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, root), NULL);
    if (!res) return 0;

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
        if (!ci) continue;
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
                if (oi->connection == XCB_RANDR_CONNECTION_CONNECTED)
                    has_connected = 1;
                free(oi);
                if (has_connected) break;
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
