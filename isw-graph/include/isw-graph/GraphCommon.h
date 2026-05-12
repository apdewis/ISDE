#ifndef _ISW_GRAPH_COMMON_H
#define _ISW_GRAPH_COMMON_H

#include <sys/types.h>
#include <sys/cdefs.h>
#include <cairo/cairo.h>
#include <ISW/DrawingArea.h>
#include <kplot.h>

#ifndef IswGraphNxAxisLabel
#define IswGraphNxAxisLabel    "xAxisLabel"
#endif
#ifndef IswGraphNyAxisLabel
#define IswGraphNyAxisLabel    "yAxisLabel"
#endif
#ifndef IswGraphNshowGrid
#define IswGraphNshowGrid     "showGrid"
#endif
#ifndef IswGraphNshowBorder
#define IswGraphNshowBorder   "showBorder"
#endif
#ifndef IswGraphNshowTicLabels
#define IswGraphNshowTicLabels "showTicLabels"
#endif
#ifndef IswGraphNxTics
#define IswGraphNxTics        "xTics"
#endif
#ifndef IswGraphNyTics
#define IswGraphNyTics        "yTics"
#endif

#ifndef IswGraphCAxisLabel
#define IswGraphCAxisLabel    "AxisLabel"
#endif
#ifndef IswGraphCShowGrid
#define IswGraphCShowGrid     "ShowGrid"
#endif
#ifndef IswGraphCShowBorder
#define IswGraphCShowBorder   "ShowBorder"
#endif
#ifndef IswGraphCShowTicLabels
#define IswGraphCShowTicLabels "ShowTicLabels"
#endif
#ifndef IswGraphCTics
#define IswGraphCTics         "Tics"
#endif

void IswGraphAttachData(Widget w, struct kdata *data,
			enum kplottype type, const struct kdatacfg *cfg);

void IswGraphAttachSmooth(Widget w, struct kdata *data,
			  enum kplottype type, const struct kdatacfg *cfg,
			  enum ksmthtype smth, const struct ksmthcfg *smthcfg);

void IswGraphDetachData(Widget w, const struct kdata *data);

void IswGraphClearData(Widget w);

void IswGraphRedraw(Widget w);

struct kplotcfg *IswGraphGetPlotCfg(Widget w);

#endif /* _ISW_GRAPH_COMMON_H */
