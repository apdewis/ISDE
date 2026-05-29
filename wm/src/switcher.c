#define _POSIX_C_SOURCE 200809L
/*
 * switcher.c — Alt+Tab window switcher OSD
 *
 * Shows window titles in MRU order using a raw XCB window + Cairo.
 * The selected entry is vertically centered, with the list wrapping.
 * Height is capped at 1/3 of the screen.
 */
#include "wm.h"

#include <stdlib.h>
#include <string.h>

#define SWITCHER_WIDTH   300   /* pixels */
#define SWITCHER_PAD       8

/* Sort comparator: descending focus_seq (MRU first) */
static int cmp_mru(const void *a, const void *b)
{
    const WmClient *ca = *(const WmClient *const *)a;
    const WmClient *cb = *(const WmClient *const *)b;
    if (ca->focus_seq > cb->focus_seq) return -1;
    if (ca->focus_seq < cb->focus_seq) return  1;
    return 0;
}

static void build_order(Wm *wm)
{
    int count = 0;
    for (WmClient *c = wm->clients; c; c = c->next) {
        if (c->transient_for) continue;
        if (c->desktop != wm->current_desktop &&
            c->desktop != 0xFFFFFFFF) continue;
        count++;
    }

    free(wm->switcher_order);
    free(wm->switcher_labels);
    wm->switcher_order = NULL;
    wm->switcher_labels = NULL;
    wm->switcher_count = 0;

    if (count == 0) return;

    wm->switcher_order  = malloc(count * sizeof(WmClient *));
    wm->switcher_labels = malloc(count * sizeof(char *));
    if (!wm->switcher_order || !wm->switcher_labels) return;

    int i = 0;
    for (WmClient *c = wm->clients; c; c = c->next) {
        if (c->transient_for) continue;
        if (c->desktop != wm->current_desktop &&
            c->desktop != 0xFFFFFFFF) continue;
        wm->switcher_order[i++] = c;
    }

    qsort(wm->switcher_order, count, sizeof(WmClient *), cmp_mru);

    for (i = 0; i < count; i++) {
        WmClient *sc = wm->switcher_order[i];
        wm->switcher_labels[i] = sc->visible_name
                                  ? sc->visible_name
                                  : sc->title ? sc->title
                                  : "(untitled)";
    }

    wm->switcher_count = count;
}

static void destroy_osd(Wm *wm)
{
    if (wm->switcher_shell) {
        xcb_destroy_window(wm->conn, wm->switcher_shell);
        wm->switcher_shell = 0;
    }
    free(wm->switcher_order);
    wm->switcher_order = NULL;
    free(wm->switcher_labels);
    wm->switcher_labels = NULL;
    wm->switcher_count = 0;
    wm->switcher_visible = 0;
}

static int row_to_index(Wm *wm, int row, int center_row)
{
    int offset = row - center_row;
    int idx = (wm->switcher_sel + offset) % wm->switcher_count;
    if (idx < 0) idx += wm->switcher_count;
    return idx;
}

static void paint_switcher(Wm *wm)
{
    if (!wm->switcher_shell) {
        return;
    }

    const IsdeColorScheme *scheme = isde_theme_current();
    int visible = wm->switcher_visible;
    int center_row = visible / 2;
    int pad = wm_scale(wm, SWITCHER_PAD);
    int row_h = wm_scale(wm, isde_font_height("general", 4));
    int osd_w = wm_scale(wm, SWITCHER_WIDTH);
    int osd_h = visible * row_h + 2 * pad;

    cairo_surface_t *surface = render_surface_for_window(
        wm->conn, wm->screen, wm->switcher_shell, osd_w, osd_h);
    if (!surface) {
        return;
    }
    cairo_t *cr = cairo_create(surface);

    unsigned int bg_color = scheme ? scheme->bg : 0x333333;
    render_fill_rect(cr, bg_color, 0, 0, osd_w, osd_h);

    int label_w = osd_w - 2 * pad;
    int font_px = row_h - wm_scale(wm, 6);
    if (font_px < 8) { font_px = 8; }

    for (int i = 0; i < visible; i++) {
        int idx = row_to_index(wm, i, center_row);
        int is_sel = (i == center_row);
        int y = pad + i * row_h;

        if (is_sel && scheme) {
            render_fill_rect(cr, scheme->active,
                             pad, y, label_w, row_h);
        }

        unsigned int fg = scheme ? scheme->fg_light : 0xFFFFFF;
        render_text(cr, wm->switcher_labels[idx], fg,
                    pad, y, label_w, row_h, font_px);
    }

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
    xcb_flush(wm->conn);
}

static void create_osd(Wm *wm)
{
    const IsdeColorScheme *scheme = isde_theme_current();

    int pm_x, pm_y, pm_w, pm_h;
    wm_get_primary_monitor(wm, &pm_x, &pm_y, &pm_w, &pm_h);

    int max_height = pm_h / 3;
    int pad = wm_scale(wm, SWITCHER_PAD);
    int row_h = wm_scale(wm, isde_font_height("general", 4));

    int max_rows = (max_height - 2 * pad) / row_h;
    if (max_rows < 1) max_rows = 1;
    int visible = wm->switcher_count;
    if (visible > max_rows) {
        visible = max_rows;
    }
    wm->switcher_visible = visible;

    int osd_h = visible * row_h + 2 * pad;
    int osd_w = wm_scale(wm, SWITCHER_WIDTH);
    int sx = pm_x + (pm_w - osd_w) / 2;
    int sy = pm_y + (pm_h - osd_h) / 2;

    uint32_t bg_pixel = wm->screen->black_pixel;
    if (scheme) {
        xcb_alloc_color_reply_t *cr = xcb_alloc_color_reply(
            wm->conn,
            xcb_alloc_color(wm->conn, wm->screen->default_colormap,
                            ((scheme->bg >> 16) & 0xFF) * 257,
                            ((scheme->bg >> 8)  & 0xFF) * 257,
                            ( scheme->bg        & 0xFF) * 257),
            NULL);
        if (cr) { bg_pixel = cr->pixel; free(cr); }
    }

    uint32_t border_pixel = wm->screen->white_pixel;
    if (scheme) {
        xcb_alloc_color_reply_t *cr = xcb_alloc_color_reply(
            wm->conn,
            xcb_alloc_color(wm->conn, wm->screen->default_colormap,
                            ((scheme->border >> 16) & 0xFF) * 257,
                            ((scheme->border >> 8)  & 0xFF) * 257,
                            ( scheme->border        & 0xFF) * 257),
            NULL);
        if (cr) { border_pixel = cr->pixel; free(cr); }
    }

    wm->switcher_shell = xcb_generate_id(wm->conn);
    uint32_t vals[] = {
        bg_pixel,
        border_pixel,
        1,
        XCB_EVENT_MASK_EXPOSURE
    };
    xcb_create_window(wm->conn, XCB_COPY_FROM_PARENT,
                      wm->switcher_shell, wm->root,
                      sx, sy, osd_w, osd_h, 1,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      wm->screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
                      XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK,
                      vals);
    xcb_map_window(wm->conn, wm->switcher_shell);
    xcb_flush(wm->conn);

    paint_switcher(wm);
}

void wm_switcher_show(Wm *wm)
{
    if (wm->switcher_active) {
        wm_switcher_next(wm);
        return;
    }

    build_order(wm);
    if (wm->switcher_count < 2) {
        free(wm->switcher_order);
        wm->switcher_order = NULL;
        free(wm->switcher_labels);
        wm->switcher_labels = NULL;
        wm->switcher_count = 0;
        return;
    }

    xcb_grab_keyboard_cookie_t ck =
        xcb_grab_keyboard(wm->conn, 1, wm->root, XCB_CURRENT_TIME,
                          XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    xcb_grab_keyboard_reply_t *reply = xcb_grab_keyboard_reply(wm->conn, ck, NULL);
    if (!reply || reply->status != XCB_GRAB_STATUS_SUCCESS) {
        free(reply);
        free(wm->switcher_order);
        wm->switcher_order = NULL;
        free(wm->switcher_labels);
        wm->switcher_labels = NULL;
        wm->switcher_count = 0;
        return;
    }
    free(reply);

    wm->switcher_active = 1;
    wm->switcher_sel = 1;

    create_osd(wm);
}

void wm_switcher_next(Wm *wm)
{
    if (!wm->switcher_active || wm->switcher_count == 0) return;

    wm->switcher_sel++;
    if (wm->switcher_sel >= wm->switcher_count) {
        wm->switcher_sel = 0;
    }

    paint_switcher(wm);
}

void wm_switcher_prev(Wm *wm)
{
    if (!wm->switcher_active || wm->switcher_count == 0) return;

    wm->switcher_sel--;
    if (wm->switcher_sel < 0) {
        wm->switcher_sel = wm->switcher_count - 1;
    }

    paint_switcher(wm);
}

void wm_switcher_commit(Wm *wm)
{
    if (!wm->switcher_active) return;

    WmClient *target = NULL;
    if (wm->switcher_sel >= 0 && wm->switcher_sel < wm->switcher_count) {
        target = wm->switcher_order[wm->switcher_sel];
    }

    xcb_ungrab_keyboard(wm->conn, XCB_CURRENT_TIME);
    destroy_osd(wm);
    wm->switcher_active = 0;

    if (target) {
        if (target->minimized) {
            wm_restore_client(wm, target);
        }
        wm_focus_client(wm, target, XCB_CURRENT_TIME);
    }
}

void wm_switcher_cancel(Wm *wm)
{
    if (!wm->switcher_active) return;

    xcb_ungrab_keyboard(wm->conn, XCB_CURRENT_TIME);
    destroy_osd(wm);
    wm->switcher_active = 0;
}
