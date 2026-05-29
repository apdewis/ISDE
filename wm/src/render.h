/*
 * render.h — Cairo/nanoSVG rendering for WM decorations
 */
#ifndef ISDE_WM_RENDER_H
#define ISDE_WM_RENDER_H

#include <xcb/xcb.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xcb.h>

/* Rasterize an SVG file to a Cairo image surface at the given size.
 * Returns NULL on failure. Caller must cairo_surface_destroy(). */
cairo_surface_t *render_svg_to_surface(const char *path, int size);

/* Paint a solid rectangle with an 0xRRGGBB color */
void render_fill_rect(cairo_t *cr, unsigned int color,
                      int x, int y, int w, int h);

/* Draw a rectangle outline of the given thickness, inside [x,x+w) x [y,y+h). */
void render_stroke_rect(cairo_t *cr, unsigned int color,
                        int x, int y, int w, int h, int thickness);

/* Paint text left-aligned within a rectangle, vertically centered.
 * Uses the default sans font. */
void render_text(cairo_t *cr, const char *text, unsigned int fg_color,
                 int x, int y, int w, int h, int font_px);

/* Paint text centered within a rectangle. */
void render_text_centered(cairo_t *cr, const char *text, unsigned int fg_color,
                          int x, int y, int w, int h, int font_px);

/* Paint an SVG icon surface centered in the given rectangle. */
void render_icon(cairo_t *cr, cairo_surface_t *icon,
                 int x, int y, int w, int h);

/* Create a Cairo surface and context for an XCB window.
 * Caller must cairo_destroy(cr) and cairo_surface_destroy(surface). */
cairo_surface_t *render_surface_for_window(xcb_connection_t *conn,
                                           xcb_screen_t *screen,
                                           xcb_window_t window,
                                           int width, int height);

#endif /* ISDE_WM_RENDER_H */
