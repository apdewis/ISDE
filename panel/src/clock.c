#define _POSIX_C_SOURCE 200809L
/*
 * clock.c — clock applet: time + date, configurable format
 */
#include "panel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


static void update_clock(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    Panel *p = (Panel *)client_data;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    char tbuf[32], dbuf[32];
    strftime(tbuf, sizeof(tbuf), p->clock_time_fmt, tm);
    strftime(dbuf, sizeof(dbuf), p->clock_date_fmt, tm);

    Arg args[20];
    IswSetArg(args[0], IswNlabel, tbuf);
    IswSetValues(p->clock_time, args, 1);
    IswSetArg(args[0], IswNlabel, dbuf);
    IswSetValues(p->clock_date, args, 1);

    /* Schedule next update — align to the next minute boundary */
    unsigned long ms = (60 - tm->tm_sec) * 1000;
    p->clock_timer = IswAppAddTimeOut(p->app, ms, update_clock, p);
}

void clock_init(Panel *p)
{
    /* Load format from config, or use defaults */
    if (!p->clock_time_fmt) {
        p->clock_time_fmt = strdup("%H:%M");
    }
    if (!p->clock_date_fmt) {
        p->clock_date_fmt = strdup("%Y-%m-%d");
    }

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
                if (tf) {
                    free(p->clock_time_fmt);
                    p->clock_time_fmt = strdup(tf);
                }
                const char *df = isde_config_string(clock_cfg,
                                                     "date_format", NULL);
                if (df) {
                    free(p->clock_date_fmt);
                    p->clock_date_fmt = strdup(df);
                }
            }
        }
        isde_config_free(cfg);
    }

    int half = PANEL_HEIGHT / 2;

    /* Derive font sizes to fit within half the panel height.
     * Each half has ~2px internal padding per side from the Label widget,
     * so available text height is (half - 4) pixels.
     * Convert back to pt: pt = px * 72 / 96 = px * 3 / 4 */
    int avail_px = half - 4;
    if (avail_px < 6) {
        avail_px = 6;
    }
    int time_pt = (avail_px * 3) / 4;
    int date_pt = time_pt > 2 ? time_pt - 2 : time_pt;

    /* Build font spec strings: "General-<size>" */
    char time_font[64], date_font[64];
    snprintf(time_font, sizeof(time_font), "Sans-%d", time_pt);
    snprintf(date_font, sizeof(date_font), "Sans-%d", date_pt);

    /* Form container in the FlexBox to stack time/date vertically */
    p->clock_box = IswVaCreateManagedWidget("clockBox", formWidgetClass,
        p->form,
        IswNborderWidth, 0,
        IswNdefaultDistance, 0,
        IswNwidth,       PANEL_CLOCK_WIDTH,
        IswNheight,      PANEL_HEIGHT,
        NULL);

    /* Time label (top half) */
    p->clock_time = IswVaCreateManagedWidget("clockTime", labelWidgetClass,
        p->clock_box,
        IswNlabel,      "00:00",
        IswNborderWidth, 0,
        IswNwidth,       PANEL_CLOCK_WIDTH,
        IswNheight,      half,
        IswNtop,         IswChainTop,
        IswNbottom,      IswChainTop,
        IswNleft,        IswChainLeft,
        IswNright,       IswChainRight,
        IswVaTypedArg, IswNfont, IswRString, time_font, strlen(time_font) + 1,
        NULL);

    /* Date label (bottom half) — below time label */
    p->clock_date = IswVaCreateManagedWidget("clockDate", labelWidgetClass,
        p->clock_box,
        IswNlabel,      "0000-00-00",
        IswNborderWidth, 0,
        IswNwidth,       PANEL_CLOCK_WIDTH,
        IswNheight,      half,
        IswNfromVert,    p->clock_time,
        IswNtop,         IswChainTop,
        IswNbottom,      IswChainBottom,
        IswNleft,        IswChainLeft,
        IswNright,       IswChainRight,
        IswVaTypedArg, IswNfont, IswRString, date_font, strlen(date_font) + 1,
        NULL);

    /* Trigger first update immediately */
    p->clock_timer = IswAppAddTimeOut(p->app, 0, update_clock, p);
}

void clock_cleanup(Panel *p)
{
    if (p->clock_timer) {
        IswRemoveTimeOut(p->clock_timer);
    }
    free(p->clock_time_fmt);
    free(p->clock_date_fmt);
    p->clock_time_fmt = NULL;
    p->clock_date_fmt = NULL;
}
