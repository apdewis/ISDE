#ifndef _ISW_GraphBarP_h
#define _ISW_GraphBarP_h

#include <isw-graph/GraphBar.h>
#include <isw-graph/GraphCommonP.h>

typedef struct {
    int makes_compiler_happy;
} GraphBarClassPart;

typedef struct _GraphBarClassRec {
    CoreClassPart          core_class;
    SimpleClassPart        simple_class;
    DrawingAreaClassPart   drawing_area_class;
    GraphBarClassPart      graph_bar_class;
} GraphBarClassRec;

extern GraphBarClassRec graphBarClassRec;

typedef struct {
    GraphBasePart  base;
} GraphBarPart;

typedef struct _GraphBarRec {
    CorePart          core;
    SimplePart        simple;
    DrawingAreaPart   drawing_area;
    GraphBarPart      graph_bar;
} GraphBarRec;

#endif /* _ISW_GraphBarP_h */
