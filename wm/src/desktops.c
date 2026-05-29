#define _POSIX_C_SOURCE 200809L
/*
 * desktops.c — virtual desktop management
 *
 * Configurable grid of desktops (rows x cols). Ctrl+Super+Arrow
 * navigates between them. An OSD popup shows the grid with the
 * active desktop highlighted.
 */
#include "wm.h"
#include "isde/isde-config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- config ---------- */

void wm_desktops_init(Wm *wm)
{
    wm->desk_rows = 1;
    wm->desk_cols = 2;

    int lo, lc, lr, lsc;
    if (isde_ewmh_get_desktop_layout(wm->ewmh, &lo, &lc, &lr, &lsc) &&
        lc > 0 && lr > 0) {
        wm->desk_cols = lc;
        wm->desk_rows = lr;
    } else {
        char errbuf[256];
        IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
        if (cfg) {
            IsdeConfigTable *root = isde_config_root(cfg);
            IsdeConfigTable *wm_tbl = isde_config_table(root, "wm");
            IsdeConfigTable *desk = wm_tbl ? isde_config_table(wm_tbl, "desktops") : NULL;
            if (desk) {
                int r = (int)isde_config_int(desk, "rows", 0);
                int c = (int)isde_config_int(desk, "columns", 0);
                if (r > 0) { wm->desk_rows = r; }
                if (c > 0) { wm->desk_cols = c; }
            }
            isde_config_free(cfg);
        }
    }

    wm->num_desktops = wm->desk_rows * wm->desk_cols;

    uint32_t prev = isde_ewmh_get_current_desktop(wm->ewmh);
    wm->current_desktop = (prev < (uint32_t)wm->num_desktops) ? prev : 0;

    isde_ewmh_set_number_of_desktops(wm->ewmh, wm->num_desktops);
    isde_ewmh_set_current_desktop(wm->ewmh, wm->current_desktop);
    xcb_flush(wm->conn);

    wm->desk_osd_timer = -1;

    fprintf(stderr, "isde-wm: desktops: %dx%d grid (%d total)\n",
            wm->desk_cols, wm->desk_rows, wm->num_desktops);
}

/* ---------- switching ---------- */

void wm_desktops_switch(Wm *wm, uint32_t desktop)
{
    if (desktop >= (uint32_t)wm->num_desktops) {
        return;
    }
    if (desktop == wm->current_desktop) {
        return;
    }

    uint32_t old = wm->current_desktop;
    wm->current_desktop = desktop;

#ifdef ISDE_COMPOSITOR
    if (wm->compositor) {
        int oldcol = old % wm->desk_cols;
        int oldrow = old / wm->desk_cols;
        int newcol = desktop % wm->desk_cols;
        int newrow = desktop / wm->desk_cols;
        int dx = (newcol > oldcol) - (newcol < oldcol);
        int dy = (newrow > oldrow) - (newrow < oldrow);
        wm_compositor_slide(wm->compositor, dx, dy);
    }
#endif

    for (WmClient *c = wm->clients; c; c = c->next) {
        if (c->desktop == 0xFFFFFFFF) {
            continue;
        }

        if (c->desktop == old && c->desktop != desktop) {
            c->hidden = 1;
            xcb_unmap_window(wm->conn, c->client);
            if (c->frame && c->mapped) {
                xcb_unmap_window(wm->conn, c->frame);
                c->mapped = 0;
            }
        } else if (c->desktop == desktop && c->desktop != old) {
            c->hidden = 0;
            xcb_map_window(wm->conn, c->client);
            if (c->frame) {
                xcb_map_window(wm->conn, c->frame);
                c->mapped = 1;
            }
        }
    }

    isde_ewmh_set_current_desktop(wm->ewmh, desktop);

    if (wm->focused && wm->focused->desktop != desktop &&
        wm->focused->desktop != 0xFFFFFFFF) {
        wm->focused->focused = 0;
        frame_apply_theme(wm, wm->focused);
        wm->focused = NULL;
        wm_ewmh_update_active(wm);
    }

    xcb_flush(wm->conn);

    wm_desktops_show_osd(wm);
}

void wm_desktops_move(Wm *wm, int dx, int dy)
{
    int col = wm->current_desktop % wm->desk_cols;
    int row = wm->current_desktop / wm->desk_cols;

    col += dx;
    row += dy;

    if (col < 0) { col = 0; }
    if (col >= wm->desk_cols) { col = wm->desk_cols - 1; }
    if (row < 0) { row = 0; }
    if (row >= wm->desk_rows) { row = wm->desk_rows - 1; }

    uint32_t target = row * wm->desk_cols + col;
    wm_desktops_switch(wm, target);
}

/* ---------- OSD popup ---------- */

#define OSD_CELL   30
#define OSD_GAP    4
#define OSD_PAD    8
#define OSD_TIMEOUT 800  /* ms */

static void osd_hide_cb(void *data)
{
    Wm *wm = (Wm *)data;
    if (wm->desk_osd) {
        xcb_unmap_window(wm->conn, wm->desk_osd);
        xcb_flush(wm->conn);
    }
    wm->desk_osd_timer = -1;
}

static void paint_osd(Wm *wm, int w, int h)
{
    if (!wm->desk_osd) {
        return;
    }

    const IsdeColorScheme *scheme = isde_theme_current();

    cairo_surface_t *surface = render_surface_for_window(
        wm->conn, wm->screen, wm->desk_osd, w, h);
    if (!surface) {
        return;
    }
    cairo_t *cr = cairo_create(surface);

    unsigned int bg = scheme ? scheme->bg : 0x333333;
    render_fill_rect(cr, bg, 0, 0, w, h);

    int cols = wm->desk_cols;
    int rows = wm->desk_rows;
    int cell = wm_scale(wm, OSD_CELL);
    int gap  = wm_scale(wm, OSD_GAP);
    int pad  = wm_scale(wm, OSD_PAD);
    int font_px = cell - wm_scale(wm, 10);
    if (font_px < 8) { font_px = 8; }

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            uint32_t idx = r * cols + c;
            int is_active = (idx == wm->current_desktop);
            int cx = pad + c * (cell + gap);
            int cy = pad + r * (cell + gap);

            if (is_active && scheme) {
                render_fill_rect(cr, scheme->active, cx, cy, cell, cell);
            } else if (scheme) {
                render_fill_rect(cr, scheme->bg_light, cx, cy, cell, cell);
            }
            if (scheme) {
                render_stroke_rect(cr, scheme->border, cx, cy, cell, cell,
                                   wm_scale(wm, 1));
            }

            char name[16];
            snprintf(name, sizeof(name), "%d", idx + 1);
            unsigned int fg = (is_active && scheme) ? scheme->fg_light
                            : scheme ? scheme->fg : 0xFFFFFF;
            render_text_centered(cr, name, fg, cx, cy, cell, cell, font_px);
        }
    }

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
    xcb_flush(wm->conn);
}

void wm_desktops_show_osd(Wm *wm)
{
    int cols = wm->desk_cols;
    int rows = wm->desk_rows;
    int cell = wm_scale(wm, OSD_CELL);
    int gap  = wm_scale(wm, OSD_GAP);
    int pad  = wm_scale(wm, OSD_PAD);
    int w = 2 * pad + cols * cell + (cols - 1) * gap;
    int h = 2 * pad + rows * cell + (rows - 1) * gap;

    int pm_x, pm_y, pm_w, pm_h;
    wm_get_primary_monitor(wm, &pm_x, &pm_y, &pm_w, &pm_h);
    int sx = pm_x + (pm_w - w) / 2;
    int sy = pm_y + (pm_h - h) / 2;

    if (wm->desk_osd_timer >= 0) {
        wm_timer_remove(wm, wm->desk_osd_timer);
        wm->desk_osd_timer = -1;
    }

    if (wm->desk_osd) {
        uint32_t vals[] = { sx, sy, w, h };
        xcb_configure_window(wm->conn, wm->desk_osd,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                             vals);
        xcb_map_window(wm->conn, wm->desk_osd);
        paint_osd(wm, w, h);
    } else {
        const IsdeColorScheme *scheme = isde_theme_current();
        uint32_t bg_pixel = wm->screen->black_pixel;
        uint32_t border_pixel = wm->screen->white_pixel;
        if (scheme) {
            xcb_alloc_color_reply_t *cr = xcb_alloc_color_reply(
                wm->conn,
                xcb_alloc_color(wm->conn, wm->screen->default_colormap,
                                ((scheme->bg >> 16) & 0xFF) * 257,
                                ((scheme->bg >> 8)  & 0xFF) * 257,
                                ( scheme->bg        & 0xFF) * 257),
                NULL);
            if (cr) { bg_pixel = cr->pixel; free(cr); }

            xcb_alloc_color_reply_t *bc = xcb_alloc_color_reply(
                wm->conn,
                xcb_alloc_color(wm->conn, wm->screen->default_colormap,
                                ((scheme->border >> 16) & 0xFF) * 257,
                                ((scheme->border >> 8)  & 0xFF) * 257,
                                ( scheme->border        & 0xFF) * 257),
                NULL);
            if (bc) { border_pixel = bc->pixel; free(bc); }
        }

        wm->desk_osd = xcb_generate_id(wm->conn);
        uint32_t vals[] = {
            bg_pixel,
            border_pixel,
            1,
            XCB_EVENT_MASK_EXPOSURE
        };
        xcb_create_window(wm->conn, XCB_COPY_FROM_PARENT,
                          wm->desk_osd, wm->root,
                          sx, sy, w, h, 1,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          wm->screen->root_visual,
                          XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
                          XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK,
                          vals);
        xcb_map_window(wm->conn, wm->desk_osd);
        xcb_flush(wm->conn);
        paint_osd(wm, w, h);
    }

    wm->desk_osd_timer = wm_timer_add(wm, OSD_TIMEOUT, osd_hide_cb, wm);
}
