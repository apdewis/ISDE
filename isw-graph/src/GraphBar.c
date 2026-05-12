#include <ISW/IntrinsicP.h>
#include <ISW/StringDefs.h>
#include <ISW/ISWInit.h>
#include <ISW/ISWRender.h>
#include <isw-graph/GraphBarP.h>

static IswResource resources[] = {
    GRAPH_BASE_RESOURCES(GraphBarRec, graph_bar.base),
};

static void Initialize(Widget, Widget, ArgList, Cardinal *);
static void Destroy(Widget);
static void Redisplay(Widget, xcb_generic_event_t *, xcb_xfixes_region_t);
static Boolean SetValues(Widget, Widget, Widget, ArgList, Cardinal *);

#define SuperClass ((DrawingAreaWidgetClass)&drawingAreaClassRec)

GraphBarClassRec graphBarClassRec = {
  {
    (WidgetClass) SuperClass,			/* superclass		  */
    "GraphBar",					/* class_name		  */
    sizeof(GraphBarRec),			/* size			  */
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

WidgetClass graphBarWidgetClass = (WidgetClass) &graphBarClassRec;

static void
Initialize(Widget request, Widget new, ArgList args, Cardinal *num_args)
{
    GraphBarWidget gbw = (GraphBarWidget) new;

    IswGraphBaseInitialize(new, &gbw->graph_bar.base);
    IswGraphBaseApplyConfig(new, &gbw->graph_bar.base);

    if (gbw->core.width == 0)  gbw->core.width = 200;
    if (gbw->core.height == 0) gbw->core.height = 150;
}

static void
Destroy(Widget w)
{
    GraphBarWidget gbw = (GraphBarWidget) w;

    IswGraphBaseDestroy(&gbw->graph_bar.base);
}

static void
Redisplay(Widget w, xcb_generic_event_t *event, xcb_xfixes_region_t region)
{
    GraphBarWidget gbw = (GraphBarWidget) w;
    DrawingAreaWidget daw = (DrawingAreaWidget) w;
    ISWRenderContext *ctx = daw->drawing_area.render_ctx;

    if (!ctx && w->core.width > 0 && w->core.height > 0 && IswIsRealized(w)) {
	ctx = daw->drawing_area.render_ctx =
	    ISWRenderCreate(w, ISW_RENDER_BACKEND_AUTO);
    }
    if (!ctx)
	return;

    ISWRenderBegin(ctx);
    IswGraphBaseDraw(w, &gbw->graph_bar.base, ctx);
    ISWRenderEnd(ctx);
}

static Boolean
SetValues(Widget current, Widget request, Widget new,
	  ArgList args, Cardinal *num_args)
{
    GraphBarWidget cur = (GraphBarWidget) current;
    GraphBarWidget neww = (GraphBarWidget) new;
    Boolean redraw;

    redraw = IswGraphBaseSetValues(new, &cur->graph_bar.base,
				   &neww->graph_bar.base);
    if (redraw)
	IswGraphBaseApplyConfig(new, &neww->graph_bar.base);

    return redraw;
}
