/*
 * preview.c — screenshot preview using DrawingArea + ISWRender
 */

#include <stdlib.h>
#include <ISW/DrawingArea.h>
#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>

#include <isde/isde-theme.h>

#include "screenshot.h"

static const Screenshot *current_ss;

static void
expose_cb(Widget w, IswPointer client_data, IswPointer call_data)
{
    (void)client_data;
    ISWDrawingCallbackData *d = (ISWDrawingCallbackData *)call_data;
    ISWRenderContext *ctx = d->render_ctx;
    if (!ctx) {
        return;
    }

    Dimension cw, ch;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgWidth(&ab, &cw);
    IswArgHeight(&ab, &ch);
    IswGetValues(w, ab.args, ab.count);

    /* Fill background */
    const IsdeColorScheme *scheme = isde_theme_current();
    unsigned int bg = scheme ? scheme->bg : 0x333333;
    ISWRenderSetColor(ctx, bg);
    ISWRenderFillRectangle(ctx, 0, 0, cw, ch);

    if (!current_ss || !current_ss->rgba) {
        return;
    }

    /* Scale image to fit while maintaining aspect ratio */
    double scale_x = (double)cw / current_ss->width;
    double scale_y = (double)ch / current_ss->height;
    double scale = scale_x < scale_y ? scale_x : scale_y;

    unsigned int dst_w = (unsigned int)(current_ss->width * scale);
    unsigned int dst_h = (unsigned int)(current_ss->height * scale);
    int dst_x = ((int)cw - (int)dst_w) / 2;
    int dst_y = ((int)ch - (int)dst_h) / 2;

    ISWRenderDrawImageRGBA(ctx, current_ss->rgba,
                           current_ss->width, current_ss->height,
                           dst_x, dst_y, dst_w, dst_h);
}

Widget
preview_create(Widget parent, char *name)
{
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgBorderWidth(&ab, 1);
    Widget canvas = IswCreateManagedWidget(name, drawingAreaWidgetClass,
                                           parent, ab.args, ab.count);

    IswAddCallback(canvas, IswNexposeCallback, expose_cb, NULL);

    return canvas;
}

void
preview_set_image(Widget preview, const Screenshot *ss)
{
    current_ss = ss;

    if (IswIsRealized(preview)) {
        xcb_connection_t *conn = IswDisplay(preview);
        xcb_clear_area(conn, 1, IswWindow(preview), 0, 0, 0, 0);
        xcb_flush(conn);
    }
}
