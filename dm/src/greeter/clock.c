#define _POSIX_C_SOURCE 200809L
/*
 * clock.c — greeter clock display (time + date)
 */
#include "greeter.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static void update_clock(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    Greeter *g = (Greeter *)client_data;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    char tbuf[32], dbuf[32];
    strftime(tbuf, sizeof(tbuf), g->clock_time_fmt, tm);
    strftime(dbuf, sizeof(dbuf), g->clock_date_fmt, tm);

    Arg args[20];
    IswSetArg(args[0], IswNlabel, tbuf);
    IswSetValues(g->clock_time, args, 1);

    IswSetArg(args[0], IswNlabel, dbuf);
    IswSetValues(g->clock_date, args, 1);

    /* Reschedule at the next minute boundary */
    unsigned long ms = (60 - tm->tm_sec) * 1000;
    g->clock_timer = IswAppAddTimeOut(g->app, ms, update_clock, g);
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
    g->clock_time = IswVaCreateManagedWidget("clockTime", labelWidgetClass,
        g->form,
        IswNlabel,         "00:00",
        IswNwidth,         g->logical_w,
        IswNheight,        eighth,
        IswNborderWidth,   0,
        IswNjustify,       IswJustifyCenter,
        IswNhorizDistance,  0,
        IswNvertDistance,   clock_y,
        IswNtop,           IswChainTop,
        IswNbottom,        IswChainTop,
        IswNleft,          IswChainLeft,
        IswNright,         IswChainLeft,
        IswVaTypedArg, IswNfont, IswRString, time_font, strlen(time_font) + 1,
        NULL);

    /* Date label */
    g->clock_date = IswVaCreateManagedWidget("clockDate", labelWidgetClass,
        g->form,
        IswNlabel,         "0000-00-00",
        IswNwidth,         g->logical_w,
        IswNborderWidth,   0,
        IswNjustify,       IswJustifyCenter,
        IswNhorizDistance,  0,
        IswNfromVert,      g->clock_time,
        IswNvertDistance,   4,
        IswNtop,           IswChainTop,
        IswNbottom,        IswChainTop,
        IswNleft,          IswChainLeft,
        IswNright,         IswChainLeft,
        IswVaTypedArg, IswNfont, IswRString, date_font, strlen(date_font) + 1,
        NULL);

    /* Start the timer immediately */
    g->clock_timer = IswAppAddTimeOut(g->app, 0, update_clock, g);
}

void greeter_clock_cleanup(Greeter *g)
{
    if (g->clock_timer) {
        IswRemoveTimeOut(g->clock_timer);
        g->clock_timer = 0;
    }
}
