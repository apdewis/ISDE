#ifndef _ISW_GraphCommonP_h
#define _ISW_GraphCommonP_h

#include <ISW/DrawingAreaP.h>
#include <isw-graph/GraphCommon.h>

typedef struct {
    /* resources */
    Pixel        foreground;
    String       x_axis_label;
    String       y_axis_label;
    Boolean      show_grid;
    Boolean      show_border;
    Boolean      show_tic_labels;
    int          x_tics;
    int          y_tics;

    /* private */
    struct kplot *plot;
    double       fg_r, fg_g, fg_b;
} GraphBasePart;

void IswGraphBaseInitialize(Widget w, GraphBasePart *gp);
void IswGraphBaseDestroy(GraphBasePart *gp);
void IswGraphBaseApplyConfig(Widget w, GraphBasePart *gp);
void IswGraphBaseDraw(Widget w, GraphBasePart *gp, ISWRenderContext *render_ctx);
Boolean IswGraphBaseSetValues(Widget w, GraphBasePart *cur, GraphBasePart *newp);

#define GRAPH_BASE_RESOURCES(type, field) \
    {IswNforeground, IswCForeground, IswRPixel, sizeof(Pixel), \
	IswOffsetOf(type, field.foreground), IswRString, IswDefaultForeground}, \
    {IswGraphNxAxisLabel, IswGraphCAxisLabel, IswRString, sizeof(String), \
	IswOffsetOf(type, field.x_axis_label), IswRString, NULL}, \
    {IswGraphNyAxisLabel, IswGraphCAxisLabel, IswRString, sizeof(String), \
	IswOffsetOf(type, field.y_axis_label), IswRString, NULL}, \
    {IswGraphNshowGrid, IswGraphCShowGrid, IswRBoolean, sizeof(Boolean), \
	IswOffsetOf(type, field.show_grid), IswRImmediate, (IswPointer)TRUE}, \
    {IswGraphNshowBorder, IswGraphCShowBorder, IswRBoolean, sizeof(Boolean), \
	IswOffsetOf(type, field.show_border), IswRImmediate, (IswPointer)TRUE}, \
    {IswGraphNshowTicLabels, IswGraphCShowTicLabels, IswRBoolean, sizeof(Boolean), \
	IswOffsetOf(type, field.show_tic_labels), IswRImmediate, (IswPointer)TRUE}, \
    {IswGraphNxTics, IswGraphCTics, IswRInt, sizeof(int), \
	IswOffsetOf(type, field.x_tics), IswRImmediate, (IswPointer)5}, \
    {IswGraphNyTics, IswGraphCTics, IswRInt, sizeof(int), \
	IswOffsetOf(type, field.y_tics), IswRImmediate, (IswPointer)5}

#endif /* _ISW_GraphCommonP_h */
