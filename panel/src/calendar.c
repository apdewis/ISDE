#define _POSIX_C_SOURCE 200809L
/*
 * calendar.c — calendar popup for the clock applet
 */
#include "panel.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ISW/IswArgMacros.h>

#define CAL_COLS     7
#define CAL_ROWS     6
#define CAL_CELLS    (CAL_COLS * CAL_ROWS)
#define CELL_W       30
#define CELL_H       24
#define HDR_H        28
#define CAL_W        (CAL_COLS * CELL_W)
#define CAL_H        (HDR_H + CELL_H + CAL_ROWS * CELL_H)

static const char *day_headers[CAL_COLS] = {
    "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"
};

static Pixel cal_color_pixel(Panel *p, unsigned int rgb)
{
    xcb_alloc_color_reply_t *reply = xcb_alloc_color_reply(
        p->conn,
        xcb_alloc_color(p->conn, p->screen->default_colormap,
                        ((rgb >> 16) & 0xFF) * 257,
                        ((rgb >> 8)  & 0xFF) * 257,
                        ( rgb        & 0xFF) * 257),
        NULL);
    if (!reply)
        return p->screen->white_pixel;
    Pixel px = reply->pixel;
    free(reply);
    return px;
}

static int days_in_month(int year, int mon)
{
    static const int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int d = dim[mon];
    if (mon == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        d = 29;
    return d;
}

/* Day of week for the 1st of the given month (0=Mon .. 6=Sun).
 * Uses Tomohiko Sakamoto's algorithm. */
static int dow_first(int year, int mon)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = year;
    if (mon < 2) y--;
    int d = (y + y/4 - y/100 + y/400 + t[mon] + 1) % 7;
    /* Convert from 0=Sun to 0=Mon */
    return (d + 6) % 7;
}

static void calendar_populate(Panel *p)
{
    char title[64];
    static const char *months[] = {
        "January","February","March","April","May","June",
        "July","August","September","October","November","December"
    };
    snprintf(title, sizeof(title), "%s %d", months[p->cal_month], p->cal_year);

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgLabel(&ab, title);
    IswSetValues(p->cal_title, ab.args, ab.count);

    int ndays = days_in_month(p->cal_year, p->cal_month);
    int start_dow = dow_first(p->cal_year, p->cal_month);

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    int today_day = -1;
    if (tm->tm_year + 1900 == p->cal_year && tm->tm_mon == p->cal_month)
        today_day = tm->tm_mday;

    const IsdeColorScheme *s = isde_theme_current();
    Pixel normal_bg = s ? cal_color_pixel(p, s->bg)       : p->screen->white_pixel;
    Pixel normal_fg = s ? cal_color_pixel(p, s->fg)       : p->screen->black_pixel;
    Pixel dim_fg    = s ? cal_color_pixel(p, s->fg_dim)   : p->screen->black_pixel;
    Pixel sel_bg    = s ? cal_color_pixel(p, s->select_bg) : p->screen->black_pixel;
    Pixel sel_fg    = s ? cal_color_pixel(p, s->select_fg) : p->screen->white_pixel;

    int prev_ndays = days_in_month(
        p->cal_month == 0 ? p->cal_year - 1 : p->cal_year,
        p->cal_month == 0 ? 11 : p->cal_month - 1);

    for (int i = 0; i < CAL_CELLS; i++) {
        char buf[4] = "";
        Pixel fg = normal_fg;
        Pixel bg = normal_bg;
        int day_num = i - start_dow + 1;

        if (i < start_dow) {
            /* Previous month overflow */
            snprintf(buf, sizeof(buf), "%d", prev_ndays - start_dow + i + 1);
            fg = dim_fg;
        } else if (day_num > ndays) {
            /* Next month overflow */
            snprintf(buf, sizeof(buf), "%d", day_num - ndays);
            fg = dim_fg;
        } else {
            snprintf(buf, sizeof(buf), "%d", day_num);
            if (day_num == today_day) {
                bg = sel_bg;
                fg = sel_fg;
            }
        }

        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, buf);
        IswArgForeground(&ab, fg);
        IswArgBackground(&ab, bg);
        IswSetValues(p->cal_days[i], ab.args, ab.count);
    }
}

static void prev_month_cb(Widget w, IswPointer client_data, IswPointer call_data)
{
    (void)w; (void)call_data;
    Panel *p = (Panel *)client_data;
    if (--p->cal_month < 0) {
        p->cal_month = 11;
        p->cal_year--;
    }
    calendar_populate(p);
}

static void next_month_cb(Widget w, IswPointer client_data, IswPointer call_data)
{
    (void)w; (void)call_data;
    Panel *p = (Panel *)client_data;
    if (++p->cal_month > 11) {
        p->cal_month = 0;
        p->cal_year++;
    }
    calendar_populate(p);
}

static void cal_button_handler(Widget w, IswPointer client_data,
                               xcb_generic_event_t *xev, Boolean *cont)
{
    (void)cont;
    Panel *p = (Panel *)client_data;
    if ((xev->response_type & ~0x80) != XCB_BUTTON_PRESS)
        return;
    xcb_button_press_event_t *bev = (xcb_button_press_event_t *)xev;
    if (bev->event == IswWindow(w))
        panel_dismiss_popup(p);
}

static void cal_key_handler(Widget w, IswPointer client_data,
                            xcb_generic_event_t *xev, Boolean *cont)
{
    (void)w; (void)cont;
    Panel *p = (Panel *)client_data;
    if ((xev->response_type & ~0x80) != XCB_KEY_PRESS)
        return;
    panel_dismiss_popup(p);
}

void calendar_init(Panel *p)
{
    const IsdeColorScheme *s = isde_theme_current();
    Pixel bg      = s ? cal_color_pixel(p, s->bg)       : p->screen->white_pixel;
    Pixel fg      = s ? cal_color_pixel(p, s->fg)       : p->screen->black_pixel;
    Pixel hdr_fg  = s ? cal_color_pixel(p, s->fg_dim)   : p->screen->black_pixel;

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgWidth(&ab, CAL_W);
    IswArgHeight(&ab, CAL_H);
    IswArgOverrideRedirect(&ab, True);
    p->cal_shell = IswCreatePopupShell("calendarShell",
                                       overrideShellWidgetClass,
                                       p->clock_box, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgDefaultDistance(&ab, 0);
    IswArgBorderWidth(&ab, 0);
    Widget form = IswCreateManagedWidget("calForm", formWidgetClass,
                                         p->cal_shell, ab.args, ab.count);

    /* Navigation header: [<] Month Year [>] */
    Widget prev_btn = IswVaCreateManagedWidget("calPrev", commandWidgetClass,
        form,
        IswNlabel,       "<",
        IswNwidth,       CELL_W,
        IswNheight,      HDR_H,
        IswNresize,      False,
        IswNborderWidth, 0,
        IswNtop,         IswChainTop,
        IswNbottom,      IswChainTop,
        IswNleft,        IswChainLeft,
        IswNright,       IswChainLeft,
        NULL);
    IswAddCallback(prev_btn, IswNcallback, prev_month_cb, p);

    p->cal_title = IswVaCreateManagedWidget("calTitle", labelWidgetClass,
        form,
        IswNlabel,       "",
        IswNwidth,       CAL_W - 2 * CELL_W,
        IswNheight,      HDR_H,
        IswNresize,      False,
        IswNborderWidth, 0,
        IswNfromHoriz,   prev_btn,
        IswNtop,         IswChainTop,
        IswNbottom,      IswChainTop,
        IswNleft,        IswChainLeft,
        IswNright,       IswChainRight,
        NULL);

    Widget next_btn = IswVaCreateManagedWidget("calNext", commandWidgetClass,
        form,
        IswNlabel,       ">",
        IswNwidth,       CELL_W,
        IswNheight,      HDR_H,
        IswNresize,      False,
        IswNborderWidth, 0,
        IswNfromHoriz,   p->cal_title,
        IswNtop,         IswChainTop,
        IswNbottom,      IswChainTop,
        IswNleft,        IswChainRight,
        IswNright,       IswChainRight,
        NULL);
    IswAddCallback(next_btn, IswNcallback, next_month_cb, p);

    /* Day-of-week header row */
    Widget prev_hdr = NULL;
    for (int i = 0; i < CAL_COLS; i++) {
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, day_headers[i]);
        IswArgWidth(&ab, CELL_W);
        IswArgHeight(&ab, CELL_H);
        IswArgResize(&ab, False);
        IswArgBorderWidth(&ab, 0);
        IswArgForeground(&ab, hdr_fg);
        IswArgBackground(&ab, bg);
        if (prev_hdr)
            IswArgFromHoriz(&ab, prev_hdr);
        IswArgFromVert(&ab, prev_btn);
        IswArgTop(&ab, IswChainTop);
        IswArgBottom(&ab, IswChainTop);
        IswArgLeft(&ab, IswChainLeft);
        IswArgRight(&ab, IswChainLeft);
        prev_hdr = IswCreateManagedWidget("dayHdr", labelWidgetClass,
                                          form, ab.args, ab.count);
    }

    /* Day cells: 6 rows x 7 columns */
    Widget first_in_prev_row = prev_hdr;  /* anchor for fromVert */
    /* We need the first widget of each row for fromVert chaining.
     * prev_hdr points to the last header; we need fromVert from any
     * header widget — they're all on the same row, so prev_hdr works. */
    Widget above_row = prev_hdr;

    for (int row = 0; row < CAL_ROWS; row++) {
        Widget prev_cell = NULL;
        for (int col = 0; col < CAL_COLS; col++) {
            int idx = row * CAL_COLS + col;
            IswArgBuilderReset(&ab);
            IswArgLabel(&ab, "");
            IswArgWidth(&ab, CELL_W);
            IswArgHeight(&ab, CELL_H);
            IswArgResize(&ab, False);
            IswArgBorderWidth(&ab, 0);
            IswArgForeground(&ab, fg);
            IswArgBackground(&ab, bg);
            if (prev_cell)
                IswArgFromHoriz(&ab, prev_cell);
            IswArgFromVert(&ab, above_row);
            IswArgTop(&ab, IswChainTop);
            IswArgBottom(&ab, IswChainTop);
            IswArgLeft(&ab, IswChainLeft);
            IswArgRight(&ab, IswChainLeft);
            p->cal_days[idx] = IswCreateManagedWidget("dayCell", labelWidgetClass,
                                                       form, ab.args, ab.count);
            prev_cell = p->cal_days[idx];
        }
        above_row = p->cal_days[row * CAL_COLS];
    }

    IswAddEventHandler(p->cal_shell, XCB_EVENT_MASK_BUTTON_PRESS, False,
                       cal_button_handler, p);
    IswAddEventHandler(p->cal_shell, XCB_EVENT_MASK_KEY_PRESS, False,
                       cal_key_handler, p);
}

void calendar_toggle(Panel *p)
{
    if (p->active_popup == p->cal_shell) {
        panel_dismiss_popup(p);
        return;
    }

    /* Reset to current month */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    p->cal_year = tm->tm_year + 1900;
    p->cal_month = tm->tm_mon;

    calendar_populate(p);

    /* Position above the clock, right-aligned to panel edge */
    double sf = ISWScaleFactor(p->toplevel);
    int log_panel_top = (int)((p->mon_y + p->mon_h) / sf + 0.5) - PANEL_HEIGHT;

    if (!IswIsRealized(p->cal_shell))
        IswRealizeWidget(p->cal_shell);

    int cal_w = p->cal_shell->core.width;
    int cal_h = p->cal_shell->core.height;

    /* Right-align: clock sits at the right edge of the panel */
    int log_mon_x = (int)(p->mon_x / sf + 0.5);
    int log_mon_w = (int)(p->mon_w / sf + 0.5);
    int cal_x = log_mon_x + log_mon_w - cal_w;
    int cal_y = log_panel_top - cal_h;

    IswConfigureWidget(p->cal_shell, cal_x, cal_y, cal_w, cal_h, 1);
    IswPopup(p->cal_shell, IswGrabNone);

    panel_show_popup(p, p->cal_shell);

    xcb_grab_keyboard(p->conn, 1, IswWindow(p->cal_shell),
                      XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC,
                      XCB_GRAB_MODE_ASYNC);
    xcb_grab_pointer(p->conn, 1, IswWindow(p->cal_shell),
                     XCB_EVENT_MASK_BUTTON_PRESS |
                     XCB_EVENT_MASK_BUTTON_RELEASE,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                     XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
    xcb_flush(p->conn);
}

void calendar_cleanup(Panel *p)
{
    (void)p;
}
