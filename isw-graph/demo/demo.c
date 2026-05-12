#include <ISW/Intrinsic.h>
#include <ISW/StringDefs.h>
#include <ISW/Shell.h>
#include <ISW/IswArgMacros.h>
#include <ISW/Box.h>
#include <ISW/Label.h>
#include <ISW/Paned.h>

#include <isw-graph/GraphLine.h>
#include <isw-graph/GraphBar.h>
#include <isw-graph/GraphScatter.h>

#include <math.h>
#include <stdlib.h>

#define LINE_POINTS  50
#define SCATTER_POINTS 80
#define BAR_BUCKETS  10

static struct kpair  sine_data[LINE_POINTS];
static struct kpair  cosine_data[LINE_POINTS];
static struct kpair  scatter_data[SCATTER_POINTS];

static void
populate_data(void)
{
    int i;

    for (i = 0; i < LINE_POINTS; i++) {
	double x = (double)i / (LINE_POINTS - 1) * 4.0 * M_PI;
	sine_data[i].x = x;
	sine_data[i].y = sin(x);
	cosine_data[i].x = x;
	cosine_data[i].y = cos(x);
    }

    srand(42);
    for (i = 0; i < SCATTER_POINTS; i++) {
	scatter_data[i].x = (double)rand() / RAND_MAX * 10.0;
	scatter_data[i].y = scatter_data[i].x * 0.5 +
	    ((double)rand() / RAND_MAX - 0.5) * 3.0;
    }
}

static Widget
create_line_graph(Widget parent)
{
    IswArgBuilder ab = IswArgBuilderInit();
    Widget graph;
    struct kdata *sin_kd, *cos_kd;
    struct kdatacfg sin_cfg, cos_cfg;

    IswArgWidth(&ab, 400);
    IswArgHeight(&ab, 300);
    ISW_ARG(&ab, IswGraphNxAxisLabel, "x");
    ISW_ARG(&ab, IswGraphNyAxisLabel, "amplitude");
    ISW_ARG(&ab, IswGraphNshowGrid, True);

    graph = IswCreateManagedWidget("lineGraph", graphLineWidgetClass,
				   parent, ab.args, ab.count);

    sin_kd = kdata_array_alloc(sine_data, LINE_POINTS);
    cos_kd = kdata_array_alloc(cosine_data, LINE_POINTS);

    kdatacfg_defaults(&sin_cfg);
    sin_cfg.line.clr.type = KPLOTCTYPE_RGBA;
    sin_cfg.line.clr.rgba[0] = 0.2;
    sin_cfg.line.clr.rgba[1] = 0.4;
    sin_cfg.line.clr.rgba[2] = 0.8;
    sin_cfg.line.clr.rgba[3] = 1.0;
    sin_cfg.line.sz = 2.0;

    kdatacfg_defaults(&cos_cfg);
    cos_cfg.line.clr.type = KPLOTCTYPE_RGBA;
    cos_cfg.line.clr.rgba[0] = 0.8;
    cos_cfg.line.clr.rgba[1] = 0.3;
    cos_cfg.line.clr.rgba[2] = 0.2;
    cos_cfg.line.clr.rgba[3] = 1.0;
    cos_cfg.line.sz = 2.0;

    IswGraphAttachData(graph, sin_kd, KPLOT_LINES, &sin_cfg);
    IswGraphAttachData(graph, cos_kd, KPLOT_LINES, &cos_cfg);

    return graph;
}

static Widget
create_bar_graph(Widget parent)
{
    IswArgBuilder ab = IswArgBuilderInit();
    Widget graph;
    struct kdata *kd;
    struct kdatacfg cfg;
    int i;

    IswArgWidth(&ab, 400);
    IswArgHeight(&ab, 300);
    ISW_ARG(&ab, IswGraphNxAxisLabel, "bucket");
    ISW_ARG(&ab, IswGraphNyAxisLabel, "count");
    ISW_ARG(&ab, IswGraphNshowGrid, True);

    graph = IswCreateManagedWidget("barGraph", graphBarWidgetClass,
				   parent, ab.args, ab.count);

    kd = kdata_hist_alloc(0.0, 10.0, BAR_BUCKETS);
    srand(123);
    for (i = 0; i < 200; i++) {
	double val = ((double)rand() / RAND_MAX + (double)rand() / RAND_MAX +
		      (double)rand() / RAND_MAX) / 3.0 * 10.0;
	kdata_hist_add(kd, val, 1.0);
    }

    kdatacfg_defaults(&cfg);
    cfg.line.clr.type = KPLOTCTYPE_RGBA;
    cfg.line.clr.rgba[0] = 0.3;
    cfg.line.clr.rgba[1] = 0.7;
    cfg.line.clr.rgba[2] = 0.3;
    cfg.line.clr.rgba[3] = 1.0;
    cfg.line.sz = 2.0;

    IswGraphAttachData(graph, kd, KPLOT_LINES, &cfg);

    return graph;
}

static Widget
create_scatter_graph(Widget parent)
{
    IswArgBuilder ab = IswArgBuilderInit();
    Widget graph;
    struct kdata *kd;
    struct kdatacfg cfg;

    IswArgWidth(&ab, 400);
    IswArgHeight(&ab, 300);
    ISW_ARG(&ab, IswGraphNxAxisLabel, "x");
    ISW_ARG(&ab, IswGraphNyAxisLabel, "y");
    ISW_ARG(&ab, IswGraphNshowGrid, True);

    graph = IswCreateManagedWidget("scatterGraph", graphScatterWidgetClass,
				   parent, ab.args, ab.count);

    kd = kdata_array_alloc(scatter_data, SCATTER_POINTS);

    kdatacfg_defaults(&cfg);
    cfg.point.clr.type = KPLOTCTYPE_RGBA;
    cfg.point.clr.rgba[0] = 0.6;
    cfg.point.clr.rgba[1] = 0.2;
    cfg.point.clr.rgba[2] = 0.7;
    cfg.point.clr.rgba[3] = 1.0;
    cfg.point.sz = 3.0;
    cfg.point.radius = 4.0;

    IswGraphAttachData(graph, kd, KPLOT_MARKS, &cfg);

    return graph;
}

int
main(int argc, char *argv[])
{
    IswAppContext app;
    Widget toplevel, paned;
    Widget line_box, bar_box, scatter_box;
    IswArgBuilder ab = IswArgBuilderInit();

    populate_data();

    toplevel = IswAppInitialize(&app, "IswGraphDemo",
				NULL, 0, &argc, argv, NULL, NULL, 0);

    IswArgWidth(&ab, 450);
    IswArgHeight(&ab, 950);
    IswArgTitle(&ab, "isw-graph Demo");
    IswArgAllowShellResize(&ab, True);
    IswSetValues(toplevel, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgOrientation(&ab, IswOrientVertical);
    paned = IswCreateManagedWidget("paned", panedWidgetClass,
				   toplevel, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgOrientation(&ab, IswOrientVertical);

    line_box = IswCreateManagedWidget("lineBox", boxWidgetClass,
				      paned, ab.args, ab.count);
    bar_box = IswCreateManagedWidget("barBox", boxWidgetClass,
				     paned, ab.args, ab.count);
    scatter_box = IswCreateManagedWidget("scatterBox", boxWidgetClass,
					 paned, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Line Graph: sin(x) and cos(x)");
    IswCreateManagedWidget("lineLabel", labelWidgetClass,
			   line_box, ab.args, ab.count);
    create_line_graph(line_box);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Bar Graph: Normal Distribution");
    IswCreateManagedWidget("barLabel", labelWidgetClass,
			   bar_box, ab.args, ab.count);
    create_bar_graph(bar_box);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Scatter Plot: Linear Trend + Noise");
    IswCreateManagedWidget("scatterLabel", labelWidgetClass,
			   scatter_box, ab.args, ab.count);
    create_scatter_graph(scatter_box);

    IswRealizeWidget(toplevel);
    IswAppMainLoop(app);

    return 0;
}
