/*
 * placement.c — smart window placement
 *
 * Transient windows (dialogs) are centered over their parent.
 * Normal windows that don't request a specific position are cascaded.
 * Windows that set USPosition or PPosition are placed where they ask.
 * All placements are clamped to the work area.
 */
#include "wm.h"

#include <string.h>

/* ---------- clamp to work area ---------- */

static void clamp_to_work_area(Wm *wm, WmClient *c)
{
    int wx, wy, ww, wh;
    wm_get_work_area(wm, &wx, &wy, &ww, &wh);

    /* Shrink client if it exceeds available space (accounting for frame) */
    int max_cw = ww - 2 * WM_BORDER_WIDTH;
    int max_ch = wh - WM_TITLE_HEIGHT - 2 * WM_BORDER_WIDTH;
    if (max_cw < 1) max_cw = 1;
    if (max_ch < 1) max_ch = 1;

    if (c->width > (uint16_t)max_cw)
        c->width = max_cw;
    if (c->height > (uint16_t)max_ch)
        c->height = max_ch;

    /* Clamp position so the frame fits within work area */
    int fw = frame_total_width(c);
    int fh = frame_total_height(c);

    if (c->x + fw > wx + ww)
        c->x = wx + ww - fw;
    if (c->y + fh > wy + wh)
        c->y = wy + wh - fh;
    if (c->x < wx)
        c->x = wx;
    if (c->y < wy)
        c->y = wy;
}

/* ---------- transient placement: center over parent ---------- */

static int place_transient(Wm *wm, WmClient *c)
{
    if (!c->transient_for)
        return 0;

    WmClient *parent = wm_find_client_by_window(wm, c->transient_for);
    if (!parent)
        return 0;

    /* Center dialog over parent's frame */
    int parent_cx = parent->x + frame_total_width(parent) / 2;
    int parent_cy = parent->y + frame_total_height(parent) / 2;

    c->x = parent_cx - frame_total_width(c) / 2;
    c->y = parent_cy - frame_total_height(c) / 2;

    clamp_to_work_area(wm, c);
    return 1;
}

/* ---------- dialog type placement: center on screen ---------- */

static int place_dialog_type(Wm *wm, WmClient *c)
{
    xcb_ewmh_connection_t *ewmh = isde_ewmh_connection(wm->ewmh);
    xcb_atom_t type = isde_ewmh_get_window_type(wm->ewmh, c->client);

    if (type != ewmh->_NET_WM_WINDOW_TYPE_DIALOG)
        return 0;

    /* No transient_for parent — center on screen work area */
    int wx, wy, ww, wh;
    wm_get_work_area(wm, &wx, &wy, &ww, &wh);

    c->x = wx + (ww - frame_total_width(c)) / 2;
    c->y = wy + (wh - frame_total_height(c)) / 2;

    clamp_to_work_area(wm, c);
    return 1;
}

/* ---------- cascade placement for normal windows ---------- */

#define CASCADE_STEP  isde_scale(24)

static void place_cascade(Wm *wm, WmClient *c)
{
    static int cascade_x, cascade_y;
    static int initialized;

    int wx, wy, ww, wh;
    wm_get_work_area(wm, &wx, &wy, &ww, &wh);

    if (!initialized) {
        cascade_x = wx;
        cascade_y = wy;
        initialized = 1;
    }

    c->x = cascade_x;
    c->y = cascade_y;

    /* Advance cascade position */
    cascade_x += CASCADE_STEP;
    cascade_y += CASCADE_STEP;

    /* Wrap if the top-left corner would go past ~2/3 of work area */
    if (cascade_x > wx + ww * 2 / 3 || cascade_y > wy + wh * 2 / 3) {
        cascade_x = wx;
        cascade_y = wy;
    }

    clamp_to_work_area(wm, c);
}

/* ---------- main entry point ---------- */

void wm_place_client(Wm *wm, WmClient *c)
{
    /* 1. Read WM_TRANSIENT_FOR */
    xcb_window_t transient = XCB_WINDOW_NONE;
    xcb_icccm_get_wm_transient_for_reply(
        wm->conn,
        xcb_icccm_get_wm_transient_for(wm->conn, c->client),
        &transient, NULL);
    c->transient_for = transient;

    /* 2. Transient windows: center over parent */
    if (place_transient(wm, c))
        return;

    /* 3. Dialog type without transient_for: center on screen */
    if (place_dialog_type(wm, c))
        return;

    /* 4. Check if client explicitly requested a position via WM_NORMAL_HINTS */
    xcb_size_hints_t hints;
    memset(&hints, 0, sizeof(hints));
    if (xcb_icccm_get_wm_normal_hints_reply(
            wm->conn,
            xcb_icccm_get_wm_normal_hints(wm->conn, c->client),
            &hints, NULL)) {
        if (hints.flags & (XCB_ICCCM_SIZE_HINT_US_POSITION |
                           XCB_ICCCM_SIZE_HINT_P_POSITION)) {
            /* Client asked for a specific position — honor it, but clamp */
            clamp_to_work_area(wm, c);
            return;
        }
    }

    /* 5. Default: cascade */
    place_cascade(wm, c);
}
