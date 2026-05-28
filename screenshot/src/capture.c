/*
 * capture.c — screen capture via xcb_get_image
 */

#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

#include "screenshot.h"

static Screenshot *
grab_rect(xcb_connection_t *conn, xcb_window_t root,
          int x, int y, unsigned int w, unsigned int h)
{
    xcb_get_image_cookie_t cookie = xcb_get_image(conn,
        XCB_IMAGE_FORMAT_Z_PIXMAP, root,
        (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, ~0u);

    xcb_get_image_reply_t *reply = xcb_get_image_reply(conn, cookie, NULL);
    if (!reply) {
        return NULL;
    }

    uint8_t *data = xcb_get_image_data(reply);
    int len = xcb_get_image_data_length(reply);
    size_t expected = (size_t)w * h * 4;

    if (len < (int)expected) {
        free(reply);
        return NULL;
    }

    Screenshot *ss = malloc(sizeof(*ss));
    if (!ss) {
        free(reply);
        return NULL;
    }

    ss->rgba = malloc(expected);
    if (!ss->rgba) {
        free(ss);
        free(reply);
        return NULL;
    }

    ss->width = w;
    ss->height = h;

    /* X11 Z_PIXMAP on 24/32-bit visuals gives BGRA; convert to RGBA */
    for (size_t i = 0; i < expected; i += 4) {
        ss->rgba[i + 0] = data[i + 2]; /* R */
        ss->rgba[i + 1] = data[i + 1]; /* G */
        ss->rgba[i + 2] = data[i + 0]; /* B */
        ss->rgba[i + 3] = 0xFF;        /* A */
    }

    free(reply);
    return ss;
}

Screenshot *
capture_fullscreen(xcb_connection_t *conn, xcb_screen_t *screen)
{
    return grab_rect(conn, screen->root, 0, 0,
                     screen->width_in_pixels, screen->height_in_pixels);
}

/* ---------- area selection ---------- */

typedef struct {
    xcb_connection_t *conn;
    xcb_screen_t     *screen;
    xcb_gcontext_t    gc;
    int               has_rect;
    int               pressed;
    int               x0, y0;
    int               x1, y1;
    int               done;
    int               cancelled;
} AreaSelect;

static void
get_rect(AreaSelect *as, int *x, int *y, int *w, int *h)
{
    *x = as->x0 < as->x1 ? as->x0 : as->x1;
    *y = as->y0 < as->y1 ? as->y0 : as->y1;
    *w = abs(as->x1 - as->x0);
    *h = abs(as->y1 - as->y0);
}

static void
draw_rubber_band(AreaSelect *as)
{
    int x, y, w, h;
    get_rect(as, &x, &y, &w, &h);
    if (w < 1 || h < 1) {
        return;
    }

    xcb_rectangle_t rect = {(int16_t)x, (int16_t)y,
                            (uint16_t)w, (uint16_t)h};
    xcb_poly_rectangle(as->conn, as->screen->root, as->gc, 1, &rect);
    xcb_flush(as->conn);
    as->has_rect = 1;
}

static void
erase_rubber_band(AreaSelect *as)
{
    if (!as->has_rect) {
        return;
    }
    int x, y, w, h;
    get_rect(as, &x, &y, &w, &h);
    if (w < 1 || h < 1) {
        return;
    }

    xcb_rectangle_t rect = {(int16_t)x, (int16_t)y,
                            (uint16_t)w, (uint16_t)h};
    xcb_poly_rectangle(as->conn, as->screen->root, as->gc, 1, &rect);
    xcb_flush(as->conn);
    as->has_rect = 0;
}

Screenshot *
capture_area(xcb_connection_t *conn, xcb_screen_t *screen)
{
    AreaSelect as = {0};
    as.conn = conn;
    as.screen = screen;

    /* GC for rubber-band: XOR on root */
    as.gc = xcb_generate_id(conn);
    uint32_t gc_mask = XCB_GC_FUNCTION | XCB_GC_FOREGROUND |
                       XCB_GC_SUBWINDOW_MODE | XCB_GC_LINE_WIDTH;
    uint32_t gc_vals[4];
    gc_vals[0] = XCB_GX_XOR;
    gc_vals[1] = 0xFFFFFF;
    gc_vals[2] = XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS;
    gc_vals[3] = 2;
    xcb_create_gc(conn, as.gc, screen->root, gc_mask, gc_vals);

    /* Crosshair cursor */
    xcb_cursor_t cursor = xcb_generate_id(conn);
    xcb_font_t font = xcb_generate_id(conn);
    xcb_open_font(conn, font, strlen("cursor"), "cursor");
    xcb_create_glyph_cursor(conn, cursor, font, font,
                            34, 35,
                            0xFFFF, 0xFFFF, 0xFFFF,
                            0, 0, 0);
    xcb_close_font(conn, font);

    /* Grab pointer on root with crosshair cursor */
    xcb_grab_pointer_cookie_t gpc = xcb_grab_pointer(conn, 0, screen->root,
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_BUTTON_MOTION,
        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
        XCB_NONE, cursor, XCB_CURRENT_TIME);
    xcb_grab_pointer_reply_t *gpr = xcb_grab_pointer_reply(conn, gpc, NULL);
    if (!gpr || gpr->status != XCB_GRAB_STATUS_SUCCESS) {
        free(gpr);
        xcb_free_gc(conn, as.gc);
        xcb_free_cursor(conn, cursor);
        return NULL;
    }
    free(gpr);

    xcb_grab_keyboard(conn, 0, screen->root, XCB_CURRENT_TIME,
                      XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    xcb_flush(conn);

    xcb_generic_event_t *ev;
    while (!as.done && (ev = xcb_wait_for_event(conn))) {
        switch (ev->response_type & ~0x80) {
        case XCB_BUTTON_PRESS: {
            xcb_button_press_event_t *bp = (xcb_button_press_event_t *)ev;
            if (bp->detail == 1) {
                as.pressed = 1;
                as.x0 = bp->root_x;
                as.y0 = bp->root_y;
                as.x1 = bp->root_x;
                as.y1 = bp->root_y;
            } else {
                as.cancelled = 1;
                as.done = 1;
            }
            break;
        }
        case XCB_MOTION_NOTIFY: {
            xcb_motion_notify_event_t *mn = (xcb_motion_notify_event_t *)ev;
            if (as.pressed) {
                erase_rubber_band(&as);
                as.x1 = mn->root_x;
                as.y1 = mn->root_y;
                draw_rubber_band(&as);
            }
            break;
        }
        case XCB_BUTTON_RELEASE: {
            xcb_button_release_event_t *br = (xcb_button_release_event_t *)ev;
            if (br->detail == 1 && as.pressed) {
                erase_rubber_band(&as);
                as.x1 = br->root_x;
                as.y1 = br->root_y;
                as.done = 1;
            }
            break;
        }
        case XCB_KEY_PRESS: {
            erase_rubber_band(&as);
            as.cancelled = 1;
            as.done = 1;
            break;
        }
        default:
            break;
        }
        free(ev);
    }

    xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
    xcb_ungrab_keyboard(conn, XCB_CURRENT_TIME);
    xcb_free_gc(conn, as.gc);
    xcb_free_cursor(conn, cursor);
    xcb_flush(conn);

    if (as.cancelled) {
        return NULL;
    }

    int x, y, w, h;
    get_rect(&as, &x, &y, &w, &h);

    if (w < 1 || h < 1) {
        return NULL;
    }

    return grab_rect(conn, screen->root, x, y, (unsigned int)w, (unsigned int)h);
}

void
screenshot_free(Screenshot *ss)
{
    if (ss) {
        free(ss->rgba);
        free(ss);
    }
}
