#include <ISW/IntrinsicP.h>
#include <ISW/StringDefs.h>
#include <ISW/ISWInit.h>
#include <ISW/ISWRender.h>
#include <isw-graph/GraphLineP.h>

static IswResource resources[] = {
    GRAPH_BASE_RESOURCES(GraphLineRec, graph_line.base),
};

static void Initialize(Widget, Widget, ArgList, Cardinal *);
static void Destroy(Widget);
static void Redisplay(Widget, IswEvent *, IswRegion);
static Boolean SetValues(Widget, Widget, Widget, ArgList, Cardinal *);

#define SuperClass ((DrawingAreaWidgetClass)&drawingAreaClassRec)

GraphLineClassRec graphLineClassRec = {
  {
    (WidgetClass) SuperClass,			/* superclass		  */
    "GraphLine",				/* class_name		  */
    sizeof(GraphLineRec),			/* size			  */
    IswInitializeWidgetSet,			/* class_initialize	  */
    NULL,					/* class_part_initialize  */
    FALSE,					/* class_inited		  */
    Initialize,					/* initialize		  */
    NULL,					/* initialize_hook	  */
    IswInheritRealize,				/* realize		  */
    NULL,					/* actions		  */
    0,						/* num_actions		  */
    resources,					/* resources		  */
    IswNumber(resources),			/* resource_count	  */
    NULLQUARK,					/* xrm_class		  */
    TRUE,					/* compress_motion	  */
    FALSE,					/* compress_exposure	  */
    TRUE,					/* compress_enterleave    */
    FALSE,					/* visible_interest	  */
    Destroy,					/* destroy		  */
    IswInheritResize,				/* resize		  */
    Redisplay,					/* expose		  */
    SetValues,					/* set_values		  */
    NULL,					/* set_values_hook	  */
    IswInheritSetValuesAlmost,			/* set_values_almost	  */
    NULL,					/* get_values_hook	  */
    NULL,					/* accept_focus		  */
    IswVersion,					/* version		  */
    NULL,					/* callback_private	  */
    NULL,					/* tm_table		  */
    IswInheritQueryGeometry,			/* query_geometry	  */
    IswInheritDisplayAccelerator,		/* display_accelerator	  */
    NULL					/* extension		  */
  },
  {
    IswInheritChangeSensitive			/* change_sensitive	  */
  },
  {
    0						/* makes_compiler_happy   */
  },
  {
    0						/* makes_compiler_happy   */
  }
};

WidgetClass graphLineWidgetClass = (WidgetClass) &graphLineClassRec;

static void
Initialize(Widget request, Widget new, ArgList args, Cardinal *num_args)
{
    GraphLineWidget glw = (GraphLineWidget) new;

    IswGraphBaseInitialize(new, &glw->graph_line.base);
    IswGraphBaseApplyConfig(new, &glw->graph_line.base);

    if (glw->core.width == 0)  glw->core.width = 200;
    if (glw->core.height == 0) glw->core.height = 150;
}

static void
Destroy(Widget w)
{
    GraphLineWidget glw = (GraphLineWidget) w;

    IswGraphBaseDestroy(&glw->graph_line.base);
}

static void
Redisplay(Widget w, IswEvent *event, IswRegion region)
{
    GraphLineWidget glw = (GraphLineWidget) w;
    DrawingAreaWidget daw = (DrawingAreaWidget) w;
    ISWRenderContext *ctx = daw->drawing_area.render_ctx;

    if (!ctx && w->core.width > 0 && w->core.height > 0 && IswIsRealized(w)) {
	ctx = daw->drawing_area.render_ctx =
	    ISWRenderCreate(w, ISW_RENDER_BACKEND_AUTO);
    }
    if (!ctx)
	return;

    ISWRenderBegin(ctx);
    IswGraphBaseDraw(w, &glw->graph_line.base, ctx);
    ISWRenderEnd(ctx);
}

static Boolean
SetValues(Widget current, Widget request, Widget new,
	  ArgList args, Cardinal *num_args)
{
    GraphLineWidget cur = (GraphLineWidget) current;
    GraphLineWidget neww = (GraphLineWidget) new;
    Boolean redraw;

    redraw = IswGraphBaseSetValues(new, &cur->graph_line.base,
				   &neww->graph_line.base);
    if (redraw)
	IswGraphBaseApplyConfig(new, &neww->graph_line.base);

    return redraw;
}
