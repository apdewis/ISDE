#define _POSIX_C_SOURCE 200809L
/*
 * clock.c — clock applet: time + date, configurable format
 */
#include "panel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


static void update_clock(XtPointer client_data, XtIntervalId *id)
{
    (void)id;
    Panel *p = (Panel *)client_data;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    char tbuf[32], dbuf[32];
    strftime(tbuf, sizeof(tbuf), p->clock_time_fmt, tm);
    strftime(dbuf, sizeof(dbuf), p->clock_date_fmt, tm);

    Arg args[20];
    XtSetArg(args[0], XtNlabel, tbuf);
    XtSetValues(p->clock_time, args, 1);
    XtSetArg(args[0], XtNlabel, dbuf);
    XtSetValues(p->clock_date, args, 1);

    /* Schedule next update — align to the next minute boundary */
    unsigned long ms = (60 - tm->tm_sec) * 1000;
    p->clock_timer = XtAppAddTimeOut(p->app, ms, update_clock, p);
}

void clock_init(Panel *p)
{
    /* Load format from config, or use defaults */
    if (!p->clock_time_fmt)
        p->clock_time_fmt = strdup("%H:%M");
    if (!p->clock_date_fmt)
        p->clock_date_fmt = strdup("%Y-%m-%d");

    /* Load configured formats from isde.toml [panel.clock] if available */
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *panel_cfg = isde_config_table(root, "panel");
        if (panel_cfg) {
            IsdeConfigTable *clock_cfg = isde_config_table(panel_cfg, "clock");
            if (clock_cfg) {
                const char *tf = isde_config_string(clock_cfg,
                                                     "time_format", NULL);
                if (tf) { free(p->clock_time_fmt); p->clock_time_fmt = strdup(tf); }
                const char *df = isde_config_string(clock_cfg,
                                                     "date_format", NULL);
                if (df) { free(p->clock_date_fmt); p->clock_date_fmt = strdup(df); }
            }
        }
        isde_config_free(cfg);
    }

    int half = PANEL_HEIGHT / 2;

    /* Time label (top half) — child of form, right of taskbar box */
    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNlabel, "00:00");              n++;
    XtSetArg(args[n], XtNborderWidth, 0);               n++;
    XtSetArg(args[n], XtNwidth, PANEL_CLOCK_WIDTH);     n++;
    XtSetArg(args[n], XtNheight, half);                  n++;
    XtSetArg(args[n], XtNfromHoriz, p->tray_box);       n++;
    XtSetArg(args[n], XtNleft, XtChainRight);           n++;
    XtSetArg(args[n], XtNright, XtChainRight);          n++;
    XtSetArg(args[n], XtNtop, XtChainTop);              n++;
    XtSetArg(args[n], XtNbottom, XtChainTop);           n++;
    p->clock_time = XtCreateManagedWidget("clockTime", labelWidgetClass,
                                          p->form, args, n);

    /* Date label (bottom half) — below time label */
    n = 0;
    XtSetArg(args[n], XtNlabel, "0000-00-00");          n++;
    XtSetArg(args[n], XtNborderWidth, 0);                n++;
    XtSetArg(args[n], XtNwidth, PANEL_CLOCK_WIDTH);      n++;
    XtSetArg(args[n], XtNheight, half);                   n++;
    XtSetArg(args[n], XtNfromVert, p->clock_time);       n++;
    XtSetArg(args[n], XtNfromHoriz, p->box);             n++;
    XtSetArg(args[n], XtNleft, XtChainRight);            n++;
    XtSetArg(args[n], XtNright, XtChainRight);           n++;
    XtSetArg(args[n], XtNtop, XtChainTop);               n++;
    XtSetArg(args[n], XtNbottom, XtChainBottom);         n++;
    p->clock_date = XtCreateManagedWidget("clockDate", labelWidgetClass,
                                          p->form, args, n);

    /* Trigger first update immediately */
    p->clock_timer = XtAppAddTimeOut(p->app, 0, update_clock, p);
}

void clock_cleanup(Panel *p)
{
    if (p->clock_timer)
        XtRemoveTimeOut(p->clock_timer);
    free(p->clock_time_fmt);
    free(p->clock_date_fmt);
    p->clock_time_fmt = NULL;
    p->clock_date_fmt = NULL;
}
