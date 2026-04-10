#define _POSIX_C_SOURCE 200809L
/*
 * clock.c — greeter clock display (time + date)
 */
#include "greeter.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static void update_clock(XtPointer client_data, XtIntervalId *id)
{
    (void)id;
    Greeter *g = (Greeter *)client_data;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    char tbuf[32], dbuf[32];
    strftime(tbuf, sizeof(tbuf), g->clock_time_fmt, tm);
    strftime(dbuf, sizeof(dbuf), g->clock_date_fmt, tm);

    Arg args[20];
    XtSetArg(args[0], XtNlabel, tbuf);
    XtSetValues(g->clock_time, args, 1);

    XtSetArg(args[0], XtNlabel, dbuf);
    XtSetValues(g->clock_date, args, 1);

    /* Reschedule at the next minute boundary */
    unsigned long ms = (60 - tm->tm_sec) * 1000;
    g->clock_timer = XtAppAddTimeOut(g->app, ms, update_clock, g);
}

void greeter_clock_init(Greeter *g)
{
    /* Time label fills the 2nd eighth of screen (1/8 to 2/8).
     * Font size ~72% of the slot height for visual fill. */
    int eighth = g->logical_h / 8;
    int time_pt = eighth * 58 / 100;
    char time_font[64], date_font[64];
    snprintf(time_font, sizeof(time_font), "Sans Bold-%d", time_pt);
    snprintf(date_font, sizeof(date_font), "Sans-%d", 14);

    int clock_y = eighth;  /* top of 2nd eighth */

    /* Time label */
    g->clock_time = XtVaCreateManagedWidget("clockTime", labelWidgetClass,
        g->form,
        XtNlabel,         "00:00",
        XtNwidth,         g->logical_w,
        XtNheight,        eighth,
        XtNborderWidth,   0,
        XtNjustify,       XtJustifyCenter,
        XtNhorizDistance,  0,
        XtNvertDistance,   clock_y,
        XtNtop,           XtChainTop,
        XtNbottom,        XtChainTop,
        XtNleft,          XtChainLeft,
        XtNright,         XtChainLeft,
        XtVaTypedArg, XtNfont, XtRString, time_font, strlen(time_font) + 1,
        NULL);

    /* Date label */
    g->clock_date = XtVaCreateManagedWidget("clockDate", labelWidgetClass,
        g->form,
        XtNlabel,         "0000-00-00",
        XtNwidth,         g->logical_w,
        XtNborderWidth,   0,
        XtNjustify,       XtJustifyCenter,
        XtNhorizDistance,  0,
        XtNfromVert,      g->clock_time,
        XtNvertDistance,   4,
        XtNtop,           XtChainTop,
        XtNbottom,        XtChainTop,
        XtNleft,          XtChainLeft,
        XtNright,         XtChainLeft,
        XtVaTypedArg, XtNfont, XtRString, date_font, strlen(date_font) + 1,
        NULL);

    /* Start the timer immediately */
    g->clock_timer = XtAppAddTimeOut(g->app, 0, update_clock, g);
}

void greeter_clock_cleanup(Greeter *g)
{
    if (g->clock_timer) {
        XtRemoveTimeOut(g->clock_timer);
        g->clock_timer = 0;
    }
}
