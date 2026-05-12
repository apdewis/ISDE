#ifndef _ISW_GraphLineP_h
#define _ISW_GraphLineP_h

#include <isw-graph/GraphLine.h>
#include <isw-graph/GraphCommonP.h>

typedef struct {
    int makes_compiler_happy;
} GraphLineClassPart;

typedef struct _GraphLineClassRec {
    CoreClassPart          core_class;
    SimpleClassPart        simple_class;
    DrawingAreaClassPart   drawing_area_class;
    GraphLineClassPart     graph_line_class;
} GraphLineClassRec;

extern GraphLineClassRec graphLineClassRec;

typedef struct {
    GraphBasePart  base;
} GraphLinePart;

typedef struct _GraphLineRec {
    CorePart          core;
    SimplePart        simple;
    DrawingAreaPart   drawing_area;
    GraphLinePart     graph_line;
} GraphLineRec;

#endif /* _ISW_GraphLineP_h */
