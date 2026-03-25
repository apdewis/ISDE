#define _POSIX_C_SOURCE 200809L
/*
 * clock.c — clock applet (rightmost panel item)
 */
#include "panel.h"

#include <stdio.h>
#include <time.h>

static void update_clock(XtPointer client_data, XtIntervalId *id)
{
    (void)id;
    Panel *p = (Panel *)client_data;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M", tm);

    Arg args[1];
    XtSetArg(args[0], XtNlabel, buf);
    XtSetValues(p->clock_label, args, 1);

    /* Schedule next update — align to the next minute boundary */
    unsigned long ms = (60 - tm->tm_sec) * 1000;
    p->clock_timer = XtAppAddTimeOut(p->app, ms, update_clock, p);
}

void clock_init(Panel *p)
{
    Arg args[4];
    Cardinal n = 0;
    XtSetArg(args[n], XtNlabel, "00:00");            n++;
    XtSetArg(args[n], XtNborderWidth, 0);             n++;
    XtSetArg(args[n], XtNwidth, 50);                  n++;
    XtSetArg(args[n], XtNheight, PANEL_HEIGHT);       n++;
    p->clock_label = XtCreateManagedWidget("clock", labelWidgetClass,
                                           p->box, args, n);

    /* Trigger first update immediately */
    p->clock_timer = XtAppAddTimeOut(p->app, 0, update_clock, p);
}

void clock_cleanup(Panel *p)
{
    if (p->clock_timer)
        XtRemoveTimeOut(p->clock_timer);
}
