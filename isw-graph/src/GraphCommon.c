#include <ISW/IntrinsicP.h>
#include <ISW/StringDefs.h>
#include <ISW/ISWRender.h>
#include <isw-graph/GraphLineP.h>
#include <isw-graph/GraphBarP.h>
#include <isw-graph/GraphScatterP.h>
#include <cairo/cairo.h>
#include <stdlib.h>
#include <string.h>

static void
resolve_foreground(Widget w, GraphBasePart *gp)
{
    xcb_connection_t *conn = w->core.display;
    xcb_colormap_t cmap = w->core.colormap;
    uint32_t pixel = (uint32_t)gp->foreground;
    xcb_query_colors_cookie_t cookie;
    xcb_query_colors_reply_t *reply;
    xcb_rgb_t *rgb;

    cookie = xcb_query_colors(conn, cmap, 1, &pixel);
    reply = xcb_query_colors_reply(conn, cookie, NULL);
    if (reply) {
	rgb = xcb_query_colors_colors(reply);
	if (xcb_query_colors_colors_length(reply) > 0) {
	    gp->fg_r = (double)(rgb[0].red >> 8) / 255.0;
	    gp->fg_g = (double)(rgb[0].green >> 8) / 255.0;
	    gp->fg_b = (double)(rgb[0].blue >> 8) / 255.0;
	}
	free(reply);
    }
}

static void
apply_fg_color(struct kplotccfg *clr, GraphBasePart *gp)
{
    clr->type = KPLOTCTYPE_RGBA;
    clr->rgba[0] = gp->fg_r;
    clr->rgba[1] = gp->fg_g;
    clr->rgba[2] = gp->fg_b;
    clr->rgba[3] = 1.0;
}

void
IswGraphBaseInitialize(Widget w, GraphBasePart *gp)
{
    gp->plot = NULL;
    gp->fg_r = gp->fg_g = gp->fg_b = 0.0;

    resolve_foreground(w, gp);

    if (gp->x_axis_label)
	gp->x_axis_label = IswNewString(gp->x_axis_label);
    if (gp->y_axis_label)
	gp->y_axis_label = IswNewString(gp->y_axis_label);
}

void
IswGraphBaseDestroy(GraphBasePart *gp)
{
    if (gp->plot) {
	kplot_free(gp->plot);
	gp->plot = NULL;
    }
    if (gp->x_axis_label)
	IswFree(gp->x_axis_label);
    if (gp->y_axis_label)
	IswFree(gp->y_axis_label);
}

void
IswGraphBaseApplyConfig(Widget w, GraphBasePart *gp)
{
    struct kplotcfg cfg;

    kplotcfg_defaults(&cfg);

    if (gp->show_grid)
	cfg.grid = GRID_ALL;
    else
	cfg.grid = 0;

    if (gp->show_border)
	cfg.border = BORDER_ALL;
    else
	cfg.border = 0;

    if (gp->show_tic_labels)
	cfg.ticlabel = TICLABEL_LEFT | TICLABEL_BOTTOM;
    else
	cfg.ticlabel = 0;

    cfg.xtics = gp->x_tics;
    cfg.ytics = gp->y_tics;
    cfg.xaxislabel = gp->x_axis_label;
    cfg.yaxislabel = gp->y_axis_label;

    apply_fg_color(&cfg.axislabelfont.clr, gp);
    apply_fg_color(&cfg.ticlabelfont.clr, gp);
    apply_fg_color(&cfg.borderline.clr, gp);
    apply_fg_color(&cfg.ticline.clr, gp);
    apply_fg_color(&cfg.gridline.clr, gp);
    cfg.gridline.clr.rgba[3] = 0.3;

    if (gp->plot)
	kplot_free(gp->plot);
    gp->plot = kplot_alloc(&cfg);
}

void
IswGraphBaseDraw(Widget w, GraphBasePart *gp, ISWRenderContext *render_ctx)
{
    cairo_t *cr;

    if (!gp->plot || !render_ctx)
	return;

    cr = ISWRenderGetCairoContext(render_ctx);
    if (!cr)
	return;

    kplot_draw(gp->plot, (double)w->core.width, (double)w->core.height, cr);
}

Boolean
IswGraphBaseSetValues(Widget w, GraphBasePart *cur, GraphBasePart *newp)
{
    Boolean redraw = FALSE;

    if (cur->foreground != newp->foreground) {
	resolve_foreground(w, newp);
	redraw = TRUE;
    }

    if (cur->x_axis_label != newp->x_axis_label) {
	if (cur->x_axis_label)
	    IswFree(cur->x_axis_label);
	newp->x_axis_label = newp->x_axis_label ?
	    IswNewString(newp->x_axis_label) : NULL;
	redraw = TRUE;
    }

    if (cur->y_axis_label != newp->y_axis_label) {
	if (cur->y_axis_label)
	    IswFree(cur->y_axis_label);
	newp->y_axis_label = newp->y_axis_label ?
	    IswNewString(newp->y_axis_label) : NULL;
	redraw = TRUE;
    }

    if (cur->show_grid != newp->show_grid ||
	cur->show_border != newp->show_border ||
	cur->show_tic_labels != newp->show_tic_labels ||
	cur->x_tics != newp->x_tics ||
	cur->y_tics != newp->y_tics)
	redraw = TRUE;

    return redraw;
}

static GraphBasePart *
get_graph_base(Widget w)
{
    WidgetClass cls = IswClass(w);

    if (cls == graphLineWidgetClass) {
	GraphLineWidget glw = (GraphLineWidget)w;
	return &glw->graph_line.base;
    }
    if (cls == graphBarWidgetClass) {
	GraphBarWidget gbw = (GraphBarWidget)w;
	return &gbw->graph_bar.base;
    }
    if (cls == graphScatterWidgetClass) {
	GraphScatterWidget gsw = (GraphScatterWidget)w;
	return &gsw->graph_scatter.base;
    }
    return NULL;
}

void
IswGraphAttachData(Widget w, struct kdata *data,
		   enum kplottype type, const struct kdatacfg *cfg)
{
    GraphBasePart *gp = get_graph_base(w);

    if (!gp)
	return;
    if (!gp->plot)
	IswGraphBaseApplyConfig(w, gp);
    kplot_attach_data(gp->plot, data, type, cfg);
}

void
IswGraphAttachSmooth(Widget w, struct kdata *data,
		     enum kplottype type, const struct kdatacfg *cfg,
		     enum ksmthtype smth, const struct ksmthcfg *smthcfg)
{
    GraphBasePart *gp = get_graph_base(w);

    if (!gp)
	return;
    if (!gp->plot)
	IswGraphBaseApplyConfig(w, gp);
    kplot_attach_smooth(gp->plot, data, type, cfg, smth, smthcfg);
}

void
IswGraphDetachData(Widget w, const struct kdata *data)
{
    GraphBasePart *gp = get_graph_base(w);

    if (!gp || !gp->plot)
	return;
    kplot_detach(gp->plot, data);
}

void
IswGraphClearData(Widget w)
{
    GraphBasePart *gp = get_graph_base(w);

    if (!gp)
	return;
    if (gp->plot) {
	kplot_free(gp->plot);
	gp->plot = NULL;
    }
    IswGraphBaseApplyConfig(w, gp);
}

void
IswGraphRedraw(Widget w)
{
    xcb_connection_t *conn;

    if (!IswIsRealized(w))
	return;

    conn = IswDisplay(w);
    xcb_clear_area(conn, 1, IswWindow(w), 0, 0, 0, 0);
    xcb_flush(conn);
}

struct kplotcfg *
IswGraphGetPlotCfg(Widget w)
{
    GraphBasePart *gp = get_graph_base(w);

    if (!gp || !gp->plot)
	return NULL;
    return kplot_get_plotcfg(gp->plot);
}
