/*
 * save.c — PNG save via Cairo, file chooser integration
 */

#include <cairo/cairo.h>

#include <isde/isde-filechooser.h>

#include "screenshot.h"

int
save_png(const Screenshot *ss, const char *path)
{
    if (!ss || !ss->rgba || !path) {
        return -1;
    }

    /* Cairo expects ARGB32 in native byte order (on little-endian: BGRA) */
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32,
                                               (int)ss->width);
    unsigned char *cairo_data = malloc((size_t)stride * ss->height);
    if (!cairo_data) {
        return -1;
    }

    for (unsigned int y = 0; y < ss->height; y++) {
        const uint8_t *src_row = ss->rgba + y * ss->width * 4;
        uint32_t *dst_row = (uint32_t *)(cairo_data + y * stride);
        for (unsigned int x = 0; x < ss->width; x++) {
            uint8_t r = src_row[x * 4 + 0];
            uint8_t g = src_row[x * 4 + 1];
            uint8_t b = src_row[x * 4 + 2];
            uint8_t a = src_row[x * 4 + 3];
            dst_row[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                         ((uint32_t)g << 8)  | (uint32_t)b;
        }
    }

    cairo_surface_t *surface = cairo_image_surface_create_for_data(
        cairo_data, CAIRO_FORMAT_ARGB32,
        (int)ss->width, (int)ss->height, stride);

    cairo_status_t status = cairo_surface_write_to_png(surface, path);

    cairo_surface_destroy(surface);
    free(cairo_data);

    return (status == CAIRO_STATUS_SUCCESS) ? 0 : -1;
}

typedef struct {
    const Screenshot *ss;
} SaveCtx;

static void
save_cb(const char *path, void *data)
{
    SaveCtx *ctx = (SaveCtx *)data;
    if (path && ctx->ss) {
        save_png(ctx->ss, path);
    }
    free(ctx);
}

void
save_dialog(Widget parent, const Screenshot *ss)
{
    SaveCtx *ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        return;
    }
    ctx->ss = ss;

    isde_filechooser_show(parent, "Save Screenshot",
                          ISDE_FILE_SAVE, NULL, "*.png",
                          save_cb, ctx);
}
