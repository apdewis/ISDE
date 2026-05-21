#define _POSIX_C_SOURCE 200809L
/*
 * pager.c — virtual desktop pager applet
 *
 * Draws a grid of desktop cells with miniature window rectangles.
 * Click a cell to switch desktop; drag a mini-window between cells
 * to move it to another desktop.
 */
#include "panel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ISW/IswArgMacros.h>
#include <cairo/cairo.h>

/* Geometry constants (logical pixels, before scaling) */
#define PAGER_GAP      2
#define PAGER_PAD      3
#define PAGER_MIN_WIN  3
#define PAGER_MAX_WINS 128

/* ---------- geometry helpers ---------- */

typedef struct {
    int x, y, w, h;
} CellRect;

static int pager_cell_w;
static int pager_cell_h;

static void compute_cell_size(Panel *p)
{
    int avail_h = PANEL_HEIGHT - 2 * PAGER_PAD -
                  (p->pager_rows - 1) * PAGER_GAP;
    pager_cell_h = avail_h / p->pager_rows;
    if (pager_cell_h < PAGER_MIN_WIN) {
        pager_cell_h = PAGER_MIN_WIN;
    }

    double aspect = (p->mon_w > 0 && p->mon_h > 0)
                    ? (double)p->mon_w / p->mon_h : 16.0 / 9.0;
    pager_cell_w = (int)(pager_cell_h * aspect + 0.5);
    if (pager_cell_w < PAGER_MIN_WIN) {
        pager_cell_w = PAGER_MIN_WIN;
    }
}

static CellRect cell_rect(Panel *p, int desktop)
{
    CellRect r;
    int col = desktop % p->pager_cols;
    int row = desktop / p->pager_cols;
    r.x = PAGER_PAD + col * (pager_cell_w + PAGER_GAP);
    r.y = PAGER_PAD + row * (pager_cell_h + PAGER_GAP);
    r.w = pager_cell_w;
    r.h = pager_cell_h;
    return r;
}

static int desktop_at_point(Panel *p, int px, int py)
{
    for (int d = 0; d < p->pager_ndesktops; d++) {
        CellRect r = cell_rect(p, d);
        if (px >= r.x && px < r.x + r.w &&
            py >= r.y && py < r.y + r.h) {
            return d;
        }
    }
    return -1;
}

/* ---------- window info cache ---------- */

typedef struct {
    xcb_window_t win;
    uint32_t     desktop;
    int16_t      x, y;
    uint16_t     w, h;
} WinInfo;

static WinInfo  pager_cache[PAGER_MAX_WINS];
static int      pager_cache_count;

static void refresh_cache(Panel *p)
{
    xcb_window_t *wins = NULL;
    int nwins = isde_ewmh_get_client_list_stacking(p->ewmh, &wins);
    if (nwins <= 0) {
        pager_cache_count = 0;
        return;
    }
    if (nwins > PAGER_MAX_WINS) {
        nwins = PAGER_MAX_WINS;
    }

    /* Batch all requests up front to avoid per-window round-trips */
    xcb_get_geometry_cookie_t geo_cookies[PAGER_MAX_WINS];
    xcb_translate_coordinates_cookie_t tr_cookies[PAGER_MAX_WINS];
    for (int i = 0; i < nwins; i++) {
        geo_cookies[i] = xcb_get_geometry(p->conn, wins[i]);
        tr_cookies[i] = xcb_translate_coordinates(p->conn, wins[i],
                                                   p->root, 0, 0);
    }

    int count = 0;
    xcb_ewmh_connection_t *ec = isde_ewmh_connection(p->ewmh);

    for (int i = 0; i < nwins; i++) {
        uint32_t desk = isde_ewmh_get_wm_desktop(p->ewmh, wins[i]);
        xcb_get_geometry_reply_t *geo =
            xcb_get_geometry_reply(p->conn, geo_cookies[i], NULL);
        xcb_translate_coordinates_reply_t *tr =
            xcb_translate_coordinates_reply(p->conn, tr_cookies[i], NULL);
        if (!geo || !tr) {
            free(geo);
            free(tr);
            continue;
        }

        xcb_atom_t wtype = isde_ewmh_get_window_type(p->ewmh, wins[i]);
        if (wtype == ec->_NET_WM_WINDOW_TYPE_DOCK) {
            free(geo);
            free(tr);
            continue;
        }

        pager_cache[count].win = wins[i];
        pager_cache[count].desktop = desk;
        pager_cache[count].x = tr->dst_x;
        pager_cache[count].y = tr->dst_y;
        pager_cache[count].w = geo->width;
        pager_cache[count].h = geo->height;
        count++;
        free(geo);
        free(tr);
    }

    free(wins);
    pager_cache_count = count;
}

static void invalidate_pager(Panel *p)
{
    IswExposeProc expose = IswClass(p->pager_canvas)->core_class.expose;
    if (expose) {
        expose(p->pager_canvas, NULL, 0);
    }
}

/* ---------- expose callback ---------- */

static void pager_expose_cb(Widget w, IswPointer client_data,
                            IswPointer call_data)
{
    Panel *p = (Panel *)client_data;
    ISWDrawingCallbackData *d = (ISWDrawingCallbackData *)call_data;
    cairo_t *cr = (cairo_t *)ISWRenderGetCairoContext(d->render_ctx);
    if (!cr) {
        return;
    }

    const IsdeColorScheme *scheme = isde_theme_current();
    double r, g, b;

    Dimension cw, ch;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgWidth(&ab, &cw);
    IswArgHeight(&ab, &ch);
    IswGetValues(w, ab.args, ab.count);

    /* Background — match panel */
    isde_color_to_rgb(scheme ? scheme->taskbar.bg : 0x333333, &r, &g, &b);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_paint(cr);

    int scr_w = p->mon_w > 0 ? p->mon_w : 1;
    int scr_h = p->mon_h > 0 ? p->mon_h : 1;
    xcb_window_t active_win = isde_ewmh_get_active_window(p->ewmh);

    for (int desk = 0; desk < p->pager_ndesktops; desk++) {
        CellRect cell = cell_rect(p, desk);
        int is_current = ((uint32_t)desk == p->pager_current);

        /* Cell fill — uniform base; active tint applied after mini-windows */
        isde_color_to_rgb(scheme ? scheme->bg_light : 0x555555, &r, &g, &b);
        cairo_set_source_rgb(cr, r, g, b);
        cairo_rectangle(cr, cell.x, cell.y, cell.w, cell.h);
        cairo_fill(cr);

        /* Cell border */
        isde_color_to_rgb(scheme ? scheme->border : 0x888888, &r, &g, &b);
        cairo_set_source_rgb(cr, r, g, b);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, cell.x + 0.5, cell.y + 0.5,
                        cell.w - 1, cell.h - 1);
        cairo_stroke(cr);

        /* Mini-windows */
        double sx = (double)cell.w / scr_w;
        double sy = (double)cell.h / scr_h;

        for (int i = 0; i < pager_cache_count; i++) {
            if (pager_cache[i].desktop != (uint32_t)desk &&
                pager_cache[i].desktop != 0xFFFFFFFF) {
                continue;
            }

            if (p->pager_dragging && pager_cache[i].win == p->pager_drag_win &&
                (uint32_t)desk == p->pager_drag_src) {
                continue;
            }

            int mx = cell.x + (int)(pager_cache[i].x * sx);
            int my = cell.y + (int)(pager_cache[i].y * sy);
            int mw = (int)(pager_cache[i].w * sx);
            int mh = (int)(pager_cache[i].h * sy);
            if (mw < PAGER_MIN_WIN) { mw = PAGER_MIN_WIN; }
            if (mh < PAGER_MIN_WIN) { mh = PAGER_MIN_WIN; }

            if (mx < cell.x) { mx = cell.x; }
            if (my < cell.y) { my = cell.y; }
            if (mx + mw > cell.x + cell.w) { mw = cell.x + cell.w - mx; }
            if (my + mh > cell.y + cell.h) { mh = cell.y + cell.h - my; }

            int is_active = (pager_cache[i].win == active_win);
            unsigned int wfill = is_active
                ? (scheme ? scheme->active : 0x4488CC)
                : (scheme ? scheme->bg_bright : 0xDDDDDD);
            isde_color_to_rgb(wfill, &r, &g, &b);
            cairo_set_source_rgb(cr, r, g, b);
            cairo_rectangle(cr, mx, my, mw, mh);
            cairo_fill(cr);

            isde_color_to_rgb(scheme ? scheme->fg : 0x000000, &r, &g, &b);
            cairo_set_source_rgb(cr, r, g, b);
            cairo_set_line_width(cr, 1.0);
            cairo_rectangle(cr, mx + 0.5, my + 0.5, mw - 1, mh - 1);
            cairo_stroke(cr);
        }

        /* Active desktop tint — drawn over mini-windows so it's always visible */
        if (is_current) {
            isde_color_to_rgb(scheme ? scheme->active : 0x4488CC, &r, &g, &b);
            cairo_set_source_rgba(cr, r, g, b, 0.3);
            cairo_rectangle(cr, cell.x, cell.y, cell.w, cell.h);
            cairo_fill(cr);
        }
    }

    /* Draw the dragged window at pointer position */
    if (p->pager_dragging) {
        for (int i = 0; i < pager_cache_count; i++) {
            if (pager_cache[i].win != p->pager_drag_win) {
                continue;
            }

            int dest = desktop_at_point(p, p->pager_drag_x, p->pager_drag_y);
            if (dest < 0) { dest = (int)p->pager_drag_src; }
            CellRect dc = cell_rect(p, dest);

            double sx = (double)dc.w / scr_w;
            double sy = (double)dc.h / scr_h;
            int mw = (int)(pager_cache[i].w * sx);
            int mh = (int)(pager_cache[i].h * sy);
            if (mw < PAGER_MIN_WIN) { mw = PAGER_MIN_WIN; }
            if (mh < PAGER_MIN_WIN) { mh = PAGER_MIN_WIN; }

            int mx = p->pager_drag_x - mw / 2;
            int my = p->pager_drag_y - mh / 2;

            isde_color_to_rgb(scheme ? scheme->active : 0x4488CC, &r, &g, &b);
            cairo_set_source_rgba(cr, r, g, b, 0.7);
            cairo_rectangle(cr, mx, my, mw, mh);
            cairo_fill(cr);
            break;
        }
    }
}

/* ---------- input callback (click + drag) ---------- */

static void pager_input_cb(Widget w, IswPointer client_data,
                           IswPointer call_data)
{
    (void)w;
    Panel *p = (Panel *)client_data;
    ISWDrawingCallbackData *d = (ISWDrawingCallbackData *)call_data;
    xcb_generic_event_t *ev = d->event;
    if (!ev) {
        return;
    }

    uint8_t type = ev->response_type & ~0x80;

    if (type == XCB_BUTTON_PRESS) {
        xcb_button_press_event_t *bev = (xcb_button_press_event_t *)ev;
        if (bev->detail != 1) {
            return;
        }

        int desk = desktop_at_point(p, bev->event_x, bev->event_y);
        if (desk < 0) {
            return;
        }

        p->pager_drag_x = bev->event_x;
        p->pager_drag_y = bev->event_y;
        p->pager_drag_src = (uint32_t)desk;
        p->pager_drag_win = XCB_WINDOW_NONE;
        p->pager_dragging = 0;

        /* Check if the press lands on a mini-window */
        CellRect cell = cell_rect(p, desk);
        int scr_w = p->mon_w > 0 ? p->mon_w : 1;
        int scr_h = p->mon_h > 0 ? p->mon_h : 1;
        double sx = (double)cell.w / scr_w;
        double sy = (double)cell.h / scr_h;

        for (int i = pager_cache_count - 1; i >= 0; i--) {
            if (pager_cache[i].desktop != (uint32_t)desk &&
                pager_cache[i].desktop != 0xFFFFFFFF) {
                continue;
            }

            int mx = cell.x + (int)(pager_cache[i].x * sx);
            int my = cell.y + (int)(pager_cache[i].y * sy);
            int mw = (int)(pager_cache[i].w * sx);
            int mh = (int)(pager_cache[i].h * sy);
            if (mw < PAGER_MIN_WIN) { mw = PAGER_MIN_WIN; }
            if (mh < PAGER_MIN_WIN) { mh = PAGER_MIN_WIN; }

            if (bev->event_x >= mx && bev->event_x < mx + mw &&
                bev->event_y >= my && bev->event_y < my + mh) {
                p->pager_drag_win = pager_cache[i].win;
                break;
            }
        }

    } else if (type == XCB_MOTION_NOTIFY && p->pager_drag_win != XCB_WINDOW_NONE) {
        xcb_motion_notify_event_t *mev = (xcb_motion_notify_event_t *)ev;
        p->pager_dragging = 1;
        p->pager_drag_x = mev->event_x;
        p->pager_drag_y = mev->event_y;
        invalidate_pager(p);

    } else if (type == XCB_BUTTON_RELEASE) {
        xcb_button_release_event_t *bev = (xcb_button_release_event_t *)ev;
        int dest = desktop_at_point(p, bev->event_x, bev->event_y);

        if (p->pager_dragging) {
            p->pager_dragging = 0;
            if (dest >= 0 && (uint32_t)dest != p->pager_drag_src) {
                isde_ewmh_request_wm_desktop(p->ewmh, p->pager_drag_win,
                                             (uint32_t)dest);
            }
            invalidate_pager(p);
        } else if (dest >= 0) {
            isde_ewmh_request_current_desktop(p->ewmh, (uint32_t)dest);
        }

        p->pager_drag_win = XCB_WINDOW_NONE;
    }
}

/* ---------- config / EWMH ---------- */

static void load_pager_config(Panel *p)
{
    p->pager_rows = 1;
    p->pager_cols = 2;

    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *wm_tbl = isde_config_table(root, "wm");
        IsdeConfigTable *desk = wm_tbl ? isde_config_table(wm_tbl, "desktops")
                                       : NULL;
        if (desk) {
            int r = (int)isde_config_int(desk, "rows", 0);
            int c = (int)isde_config_int(desk, "columns", 0);
            if (r > 0) { p->pager_rows = r; }
            if (c > 0) { p->pager_cols = c; }
        }
        isde_config_free(cfg);
    }

    p->pager_ndesktops = p->pager_rows * p->pager_cols;
}

/* ---------- init / update / cleanup ---------- */

void pager_init(Panel *p)
{
    load_pager_config(p);

    isde_ewmh_set_desktop_layout(p->ewmh,
                                 XCB_EWMH_WM_ORIENTATION_HORZ,
                                 p->pager_cols, p->pager_rows,
                                 XCB_EWMH_WM_TOPLEFT);

    p->pager_current = isde_ewmh_get_current_desktop(p->ewmh);
    compute_cell_size(p);
    refresh_cache(p);

    int total_w = 2 * PAGER_PAD +
                  p->pager_cols * pager_cell_w +
                  (p->pager_cols - 1) * PAGER_GAP;

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgWidth(&ab, total_w);
    IswArgHeight(&ab, PANEL_HEIGHT);
    IswArgBorderWidth(&ab, 0);
    p->pager_canvas = IswCreateManagedWidget("pager", drawingAreaWidgetClass,
                                             p->form, ab.args, ab.count);

    IswAddCallback(p->pager_canvas, IswNexposeCallback,
                   pager_expose_cb, p);
    IswAddCallback(p->pager_canvas, IswNinputCallback,
                   pager_input_cb, p);
}

void pager_update(Panel *p)
{
    if (!p->pager_canvas) {
        return;
    }

    uint32_t cur = isde_ewmh_get_current_desktop(p->ewmh);
    int desk_changed = (cur != p->pager_current);
    p->pager_current = cur;

    int old_count = pager_cache_count;
    WinInfo old_first;
    WinInfo old_last;
    if (old_count > 0) {
        old_first = pager_cache[0];
        old_last  = pager_cache[old_count - 1];
    }

    refresh_cache(p);

    int changed = desk_changed ||
                  (pager_cache_count != old_count);
    if (!changed && pager_cache_count > 0) {
        changed = memcmp(&old_first, &pager_cache[0], sizeof(WinInfo)) != 0 ||
                  memcmp(&old_last, &pager_cache[pager_cache_count - 1],
                         sizeof(WinInfo)) != 0;
    }

    if (changed) {
        invalidate_pager(p);
    }
}

void pager_cleanup(Panel *p)
{
    free(p->pager_wins);
    p->pager_wins = NULL;
    p->pager_nwins = 0;
    pager_cache_count = 0;
}
