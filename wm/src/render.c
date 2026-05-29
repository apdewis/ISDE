#define _POSIX_C_SOURCE 200809L
/*
 * render.c — Cairo/nanoSVG rendering for WM decorations
 */
#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"

static void color_to_cairo(unsigned int rgb, double *r, double *g, double *b)
{
    *r = ((rgb >> 16) & 0xFF) / 255.0;
    *g = ((rgb >> 8)  & 0xFF) / 255.0;
    *b = ( rgb        & 0xFF) / 255.0;
}

cairo_surface_t *render_svg_to_surface(const char *path, int size)
{
    if (!path || size < 1) {
        return NULL;
    }

    NSVGimage *svg = nsvgParseFromFile(path, "px", 96.0f);
    if (!svg) {
        return NULL;
    }

    float scale = (float)size / (svg->width > svg->height ? svg->width : svg->height);

    unsigned char *pixels = malloc(size * size * 4);
    if (!pixels) {
        nsvgDelete(svg);
        return NULL;
    }
    memset(pixels, 0, size * size * 4);

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) {
        free(pixels);
        nsvgDelete(svg);
        return NULL;
    }

    float offset_x = (size - svg->width * scale) / 2.0f;
    float offset_y = (size - svg->height * scale) / 2.0f;
    nsvgRasterize(rast, svg, offset_x, offset_y, scale, pixels, size, size, size * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(svg);

    /* nanoSVG outputs RGBA; Cairo expects ARGB32 (premultiplied, native order).
     * On little-endian that's BGRA in memory. Convert in place. */
    for (int i = 0; i < size * size; i++) {
        unsigned char *p = pixels + i * 4;
        unsigned char r = p[0], g = p[1], b = p[2], a = p[3];
        /* Premultiply */
        r = (r * a + 127) / 255;
        g = (g * a + 127) / 255;
        b = (b * a + 127) / 255;
        p[0] = b;
        p[1] = g;
        p[2] = r;
        p[3] = a;
    }

    cairo_surface_t *surface = cairo_image_surface_create_for_data(
        pixels, CAIRO_FORMAT_ARGB32, size, size, size * 4);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        free(pixels);
        return NULL;
    }

    /* Attach pixel data so it's freed when the surface is destroyed */
    static cairo_user_data_key_t key;
    cairo_surface_set_user_data(surface, &key, pixels, free);
    return surface;
}

void render_fill_rect(cairo_t *cr, unsigned int color,
                      int x, int y, int w, int h)
{
    double r, g, b;
    color_to_cairo(color, &r, &g, &b);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
}

void render_text(cairo_t *cr, const char *text, unsigned int fg_color,
                 int x, int y, int w, int h, int font_px)
{
    if (!text || !text[0]) {
        return;
    }

    double r, g, b;
    color_to_cairo(fg_color, &r, &g, &b);

    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_px);

    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    double ty = y + (h - fe.height) / 2.0 + fe.ascent;

    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);

    cairo_move_to(cr, x + 4, ty);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_show_text(cr, text);
    cairo_restore(cr);
}

void render_text_centered(cairo_t *cr, const char *text, unsigned int fg_color,
                          int x, int y, int w, int h, int font_px)
{
    if (!text || !text[0]) {
        return;
    }

    double r, g, b;
    color_to_cairo(fg_color, &r, &g, &b);

    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_px);

    cairo_text_extents_t te;
    cairo_text_extents(cr, text, &te);
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);

    double tx = x + (w - te.width) / 2.0 - te.x_bearing;
    double ty = y + (h - fe.height) / 2.0 + fe.ascent;

    cairo_move_to(cr, tx, ty);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_show_text(cr, text);
}

void render_icon(cairo_t *cr, cairo_surface_t *icon,
                 int x, int y, int w, int h)
{
    if (!icon) {
        return;
    }

    int iw = cairo_image_surface_get_width(icon);
    int ih = cairo_image_surface_get_height(icon);
    double ix = x + (w - iw) / 2.0;
    double iy = y + (h - ih) / 2.0;

    cairo_set_source_surface(cr, icon, ix, iy);
    cairo_paint(cr);
}

cairo_surface_t *render_surface_for_window(xcb_connection_t *conn,
                                           xcb_screen_t *screen,
                                           xcb_window_t window,
                                           int width, int height)
{
    xcb_visualtype_t *visual = NULL;
    xcb_depth_iterator_t depth_iter = xcb_screen_allowed_depths_iterator(screen);
    for (; depth_iter.rem; xcb_depth_next(&depth_iter)) {
        xcb_visualtype_iterator_t vis_iter =
            xcb_depth_visuals_iterator(depth_iter.data);
        for (; vis_iter.rem; xcb_visualtype_next(&vis_iter)) {
            if (vis_iter.data->visual_id == screen->root_visual) {
                visual = vis_iter.data;
                break;
            }
        }
        if (visual) {
            break;
        }
    }

    if (!visual) {
        return NULL;
    }

    return cairo_xcb_surface_create(conn, window, visual, width, height);
}
