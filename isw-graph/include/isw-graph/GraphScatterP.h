#ifndef _ISW_GraphScatterP_h
#define _ISW_GraphScatterP_h

#include <isw-graph/GraphScatter.h>
#include <isw-graph/GraphCommonP.h>

typedef struct {
    int makes_compiler_happy;
} GraphScatterClassPart;

typedef struct _GraphScatterClassRec {
    CoreClassPart          core_class;
    SimpleClassPart        simple_class;
    DrawingAreaClassPart   drawing_area_class;
    GraphScatterClassPart  graph_scatter_class;
} GraphScatterClassRec;

extern GraphScatterClassRec graphScatterClassRec;

typedef struct {
    GraphBasePart  base;
} GraphScatterPart;

typedef struct _GraphScatterRec {
    CorePart          core;
    SimplePart        simple;
    DrawingAreaPart   drawing_area;
    GraphScatterPart  graph_scatter;
} GraphScatterRec;

#endif /* _ISW_GraphScatterP_h */
