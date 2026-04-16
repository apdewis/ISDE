#define _POSIX_C_SOURCE 200809L
/*
 * popup.c — volume popup and right-click menu for isde-tray-audio
 *
 * Left-click: tabbed popup with Outputs / Applications tabs, each
 * containing per-node horizontal sliders and mute toggles.
 * Right-click: SimpleMenu for output sink selection.
 */
#include "tray-audio.h"

#include <ISW/IntrinsicP.h>
#include <ISW/ISWRender.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ================================================================
 * Volume popup
 * ================================================================ */

/* Per-row data for slider/mute callbacks */
typedef struct VolumeRow {
    TrayAudio  *ta;
    uint32_t    node_id;
    Widget      slider;
    Widget      mute_btn;
} VolumeRow;

static VolumeRow *rows = NULL;
static int nrows = 0;
static int cap_rows = 0;

static VolumeRow *alloc_row(TrayAudio *ta, uint32_t node_id)
{
    if (nrows >= cap_rows) {
        cap_rows = cap_rows ? cap_rows * 2 : 16;
        rows = realloc(rows, cap_rows * sizeof(VolumeRow));
    }
    VolumeRow *r = &rows[nrows++];
    r->ta = ta;
    r->node_id = node_id;
    r->slider = NULL;
    r->mute_btn = NULL;
    return r;
}

/* ---------- callbacks ---------- */

static void on_slider_changed(Widget w, IswPointer client_data,
                              IswPointer call_data)
{
    (void)w;
    VolumeRow *r = (VolumeRow *)client_data;
    IswSliderCallbackData *cd = (IswSliderCallbackData *)call_data;

    float vol = (float)cd->value / 100.0f;
    ta_pw_set_volume(r->ta, r->node_id, vol);
}

static void on_mute_toggled(Widget w, IswPointer client_data,
                            IswPointer call_data)
{
    (void)call_data;
    VolumeRow *r = (VolumeRow *)client_data;

    Arg args[20];
    Cardinal n = 0;
    Boolean state = False;
    IswSetArg(args[n], IswNstate, (IswArgVal)&state); n++;
    IswGetValues(w, args, n);

    ta_pw_set_mute(r->ta, r->node_id, state ? 1 : 0);
}

/* ---------- build a single volume row ---------- */

static Widget build_volume_row(TrayAudio *ta, Widget parent,
                               Widget above,
                               const char *label_text,
                               uint32_t node_id,
                               float volume, int muted)
{
    Arg args[20];
    Cardinal n;
    VolumeRow *row = alloc_row(ta, node_id);

    /* Label */
    n = 0;
    IswSetArg(args[n], IswNlabel, label_text); n++;
    IswSetArg(args[n], IswNborderWidth, 0); n++;
    IswSetArg(args[n], IswNfromVert, above); n++;
    IswSetArg(args[n], IswNleft, IswChainLeft); n++;
    IswSetArg(args[n], IswNright, IswChainLeft); n++;
    IswSetArg(args[n], IswNwidth, 120); n++;
    IswSetArg(args[n], IswNjustify, IswJustifyLeft); n++;
    IswSetArg(args[n], IswNhorizDistance, 8); n++;
    IswSetArg(args[n], IswNvertDistance, 4); n++;
    Widget lbl = IswCreateManagedWidget("volLabel", labelWidgetClass,
                                        parent, args, n);

    /* Slider */
    n = 0;
    IswSetArg(args[n], IswNfromVert, above); n++;
    IswSetArg(args[n], IswNfromHoriz, lbl); n++;
    IswSetArg(args[n], IswNleft, IswChainLeft); n++;
    IswSetArg(args[n], IswNright, IswChainRight); n++;
    IswSetArg(args[n], IswNminimumValue, 0); n++;
    IswSetArg(args[n], IswNmaximumValue, 100); n++;
    IswSetArg(args[n], IswNsliderValue, (int)(volume * 100.0f + 0.5f)); n++;
    IswSetArg(args[n], IswNshowValue, False); n++;
    IswSetArg(args[n], IswNwidth, 120); n++;
    IswSetArg(args[n], IswNhorizDistance, 4); n++;
    IswSetArg(args[n], IswNvertDistance, 4); n++;
    Widget sl = IswCreateManagedWidget("volSlider", sliderWidgetClass,
                                       parent, args, n);
    IswAddCallback(sl, IswNvalueChanged, on_slider_changed, row);
    row->slider = sl;

    /* Mute toggle */
    n = 0;
    IswSetArg(args[n], IswNlabel, "M"); n++;
    IswSetArg(args[n], IswNfromVert, above); n++;
    IswSetArg(args[n], IswNfromHoriz, sl); n++;
    IswSetArg(args[n], IswNleft, IswChainRight); n++;
    IswSetArg(args[n], IswNright, IswChainRight); n++;
    IswSetArg(args[n], IswNstate, muted ? True : False); n++;
    IswSetArg(args[n], IswNwidth, 24); n++;
    IswSetArg(args[n], IswNhorizDistance, 4); n++;
    IswSetArg(args[n], IswNvertDistance, 4); n++;
    Widget mb = IswCreateManagedWidget("volMute", toggleWidgetClass,
                                       parent, args, n);
    IswAddCallback(mb, IswNcallback, on_mute_toggled, row);
    row->mute_btn = mb;

    return lbl;  /* Return first widget of this row for fromVert chaining */
}

/* ---------- destroy all children of a Form ---------- */

static void clear_form_children(Widget form)
{
    WidgetList children;
    Cardinal num;
    Arg args[20];
    Cardinal n = 0;

    IswSetArg(args[n], IswNchildren, &children); n++;
    IswSetArg(args[n], IswNnumChildren, &num); n++;
    IswGetValues(form, args, n);

    /* Destroy in reverse to avoid index shifting */
    for (int i = (int)num - 1; i >= 0; i--)
        IswDestroyWidget(children[i]);
}

/* ---------- build tab pages ---------- */

static void build_output_content(TrayAudio *ta)
{
    clear_form_children(ta->output_page);
    Widget above = NULL;

    if (ta->nsinks == 0) {
        Arg args[20];
        Cardinal n = 0;
        const IsdeColorScheme *scheme = isde_theme_current();
        IswSetArg(args[n], IswNlabel, "No audio outputs"); n++;
        IswSetArg(args[n], IswNborderWidth, 0); n++;
        if (scheme) {
            IswSetArg(args[n], IswNforeground, scheme->fg_dim); n++;
        }
        IswSetArg(args[n], IswNhorizDistance, 8); n++;
        IswSetArg(args[n], IswNvertDistance, 8); n++;
        IswCreateManagedWidget("noOutputs", labelWidgetClass,
                              ta->output_page, args, n);
        return;
    }

    for (int i = 0; i < ta->nsinks; i++) {
        SinkInfo *s = &ta->sinks[i];
        char label[280];
        if (s->is_default)
            snprintf(label, sizeof(label), "* %s", s->name);
        else
            snprintf(label, sizeof(label), "  %s", s->name);

        above = build_volume_row(ta, ta->output_page, above,
                                 label, s->id, s->volume, s->muted);
    }
}

static void build_app_content(TrayAudio *ta)
{
    clear_form_children(ta->app_page);
    Widget above = NULL;

    if (ta->nstreams == 0) {
        Arg args[20];
        Cardinal n = 0;
        const IsdeColorScheme *scheme = isde_theme_current();
        IswSetArg(args[n], IswNlabel, "No applications playing"); n++;
        IswSetArg(args[n], IswNborderWidth, 0); n++;
        if (scheme) {
            IswSetArg(args[n], IswNforeground, scheme->fg_dim); n++;
        }
        IswSetArg(args[n], IswNhorizDistance, 8); n++;
        IswSetArg(args[n], IswNvertDistance, 8); n++;
        IswCreateManagedWidget("noStreams", labelWidgetClass,
                              ta->app_page, args, n);
        return;
    }

    for (int i = 0; i < ta->nstreams; i++) {
        StreamInfo *s = &ta->streams[i];
        above = build_volume_row(ta, ta->app_page, above,
                                 s->name, s->id, s->volume, s->muted);
    }
}

/* ---------- tab switch callback ---------- */

static void on_tab_changed(Widget w, IswPointer client_data,
                           IswPointer call_data)
{
    (void)w;
    TrayAudio *ta = (TrayAudio *)client_data;
    TabsCallbackStruct *tcs = (TabsCallbackStruct *)call_data;

    /* Reset row allocations — both pages rebuilt fresh */
    nrows = 0;

    if (tcs->child == ta->output_page)
        build_output_content(ta);
    else if (tcs->child == ta->app_page)
        build_app_content(ta);
}

/* ---------- popup dismiss ---------- */

/* Dismiss mask — same as MenuBar.c */
#define POPUP_DISMISS_MASK (XCB_EVENT_MASK_BUTTON_PRESS)

/* Event handler on the popup shell.  With owner_events=True on the
 * pointer grab, clicks outside all our windows are delivered to the
 * grab window (popup shell) with coordinates relative to it.
 * Clicks inside child widgets are delivered to those children and
 * never reach this handler.  So any button press that arrives here
 * with coordinates outside the popup bounds is an outside click. */
static void popup_outside_handler(Widget w, IswPointer closure,
                                  xcb_generic_event_t *event,
                                  Boolean *cont)
{
    (void)cont;
    TrayAudio *ta = (TrayAudio *)closure;
    uint8_t type = event->response_type & 0x7f;

    if (type != XCB_BUTTON_PRESS)
        return;
    if (!ta->popup_visible || !ta->popup_shell)
        return;

    xcb_button_press_event_t *ev = (xcb_button_press_event_t *)event;
    double sf = ISWScaleFactor(w);
    int pw = (int)(w->core.width * sf + 0.5);
    int ph = (int)(w->core.height * sf + 0.5);

    if (ev->event_x < 0 || ev->event_y < 0 ||
        ev->event_x >= pw || ev->event_y >= ph) {
        ta_popup_hide(ta);
    }
}

/* ---------- position popup above tray icon ---------- */

static void position_popup(TrayAudio *ta)
{
    if (!ta->tray_icon)
        return;

    xcb_connection_t *conn = IswDisplay(ta->toplevel);
    xcb_window_t icon_win = IswTrayIconGetWindow(ta->tray_icon);
    xcb_window_t root = IswScreen(ta->toplevel)->root;

    xcb_translate_coordinates_cookie_t cookie =
        xcb_translate_coordinates(conn, icon_win, root, 0, 0);
    xcb_translate_coordinates_reply_t *reply =
        xcb_translate_coordinates_reply(conn, cookie, NULL);

    if (!reply)
        return;

    double sf = ISWScaleFactor(ta->toplevel);
    int phys_w = (int)(ta->popup_shell->core.width * sf + 0.5);
    int phys_h = (int)(ta->popup_shell->core.height * sf + 0.5);
    int phys_bw = (int)(ta->popup_shell->core.border_width * sf + 0.5);
    int total_w = phys_w + 2 * phys_bw;
    int total_h = phys_h + 2 * phys_bw;

    int scr_w = IswScreen(ta->toplevel)->width_in_pixels;

    int x = reply->dst_x;
    int y = reply->dst_y - total_h;

    if (x + total_w > scr_w)
        x = scr_w - total_w;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    uint32_t vals[] = { (uint32_t)x, (uint32_t)y };
    xcb_configure_window(conn, IswWindow(ta->popup_shell),
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                         vals);
    xcb_flush(conn);
    free(reply);
}

/* ---------- public popup API ---------- */

void ta_popup_init(TrayAudio *ta)
{
    /* Shell created fresh each time in ta_popup_show */
    ta->popup_shell = NULL;
    ta->tabs = NULL;
    ta->output_page = NULL;
    ta->app_page = NULL;
    ta->popup_visible = 0;
}

void ta_popup_show(TrayAudio *ta)
{
    if (ta->popup_visible) {
        ta_popup_hide(ta);
        return;
    }

    /* Destroy old popup if any */
    if (ta->popup_shell) {
        IswDestroyWidget(ta->popup_shell);
        ta->popup_shell = NULL;
    }

    /* Reset row allocations */
    nrows = 0;

    Arg args[20];
    Cardinal n;

    /* Override shell for popup */
    n = 0;
    IswSetArg(args[n], IswNwidth, 320); n++;
    IswSetArg(args[n], IswNheight, 200); n++;
    IswSetArg(args[n], IswNborderWidth, 1); n++;
    ta->popup_shell = IswCreatePopupShell("audioPopup",
                                          overrideShellWidgetClass,
                                          ta->toplevel, args, n);

    /* Tabs container */
    n = 0;
    ta->tabs = IswCreateManagedWidget("tabs", tabsWidgetClass,
                                      ta->popup_shell, args, n);

    /* Outputs tab */
    n = 0;
    IswSetArg(args[n], IswNtabLabel, "Outputs"); n++;
    IswSetArg(args[n], IswNdefaultDistance, 0); n++;
    ta->output_page = IswCreateManagedWidget("outputPage",
                                             formWidgetClass,
                                             ta->tabs, args, n);

    /* Applications tab — created empty, populated on tab switch */
    n = 0;
    IswSetArg(args[n], IswNtabLabel, "Applications"); n++;
    IswSetArg(args[n], IswNdefaultDistance, 0); n++;
    ta->app_page = IswCreateManagedWidget("appPage",
                                          formWidgetClass,
                                          ta->tabs, args, n);

    /* Show the outputs tab by default */
    IswTabsSetTop(ta->tabs, ta->output_page);
    IswAddCallback(ta->tabs, IswNtabCallback, on_tab_changed, ta);

    /* Pop up without Xt grab — we handle dismissal ourselves,
     * matching the MenuBar.c pattern. */
    IswPopup(ta->popup_shell, IswGrabNone);

    /* Now populate the visible page */
    build_output_content(ta);

    /* Server pointer grab with owner_events=True so events still
     * reach the natural target window (child widgets work normally)
     * but clicks outside any of our windows come to the grab window. */
    {
        xcb_connection_t *conn = IswDisplay(ta->toplevel);
        xcb_grab_pointer(conn, True, IswWindow(ta->popup_shell),
                         XCB_EVENT_MASK_BUTTON_PRESS |
                         XCB_EVENT_MASK_BUTTON_RELEASE,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                         XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
        xcb_flush(conn);
    }

    /* Install click-outside handler on the popup shell — with
     * owner_events=True, clicks outside all our windows are
     * delivered to the grab window (the popup shell). */
    IswAddEventHandler(ta->popup_shell, POPUP_DISMISS_MASK, False,
                       popup_outside_handler, ta);

    position_popup(ta);
    ta->popup_visible = 1;
}

void ta_popup_hide(TrayAudio *ta)
{
    if (!ta->popup_visible)
        return;

    if (ta->popup_shell)
        IswRemoveEventHandler(ta->popup_shell, POPUP_DISMISS_MASK, False,
                              popup_outside_handler, ta);

    xcb_ungrab_pointer(IswDisplay(ta->toplevel), XCB_CURRENT_TIME);
    xcb_flush(IswDisplay(ta->toplevel));

    if (ta->popup_shell)
        IswPopdown(ta->popup_shell);

    ta->popup_visible = 0;
}

void ta_popup_update(TrayAudio *ta)
{
    if (!ta->popup_visible)
        return;

    /* Update slider values and mute states without rebuilding.
     * Walk the row list and sync values. */
    for (int i = 0; i < nrows; i++) {
        VolumeRow *r = &rows[i];

        float vol = 0.0f;
        int muted = 0;

        SinkInfo *sink = ta_find_sink(r->ta, r->node_id);
        if (sink) {
            vol = sink->volume;
            muted = sink->muted;
        } else {
            StreamInfo *stream = ta_find_stream(r->ta, r->node_id);
            if (stream) {
                vol = stream->volume;
                muted = stream->muted;
            }
        }

        if (r->slider)
            IswSliderSetValue(r->slider, (int)(vol * 100.0f + 0.5f));

        if (r->mute_btn) {
            Arg args[20];
            Cardinal n = 0;
            IswSetArg(args[n], IswNstate, muted ? True : False); n++;
            IswSetValues(r->mute_btn, args, n);
        }
    }
}

void ta_popup_cleanup(TrayAudio *ta)
{
    if (ta->popup_shell) {
        IswDestroyWidget(ta->popup_shell);
        ta->popup_shell = NULL;
    }
    ta->tabs = NULL;
    ta->output_page = NULL;
    ta->app_page = NULL;
    ta->popup_visible = 0;

    free(rows);
    rows = NULL;
    nrows = 0;
    cap_rows = 0;
}

/* ================================================================
 * Right-click menu (output selection)
 * ================================================================ */

typedef struct MenuAction {
    TrayAudio *ta;
    uint32_t   sink_id;
} MenuAction;

static MenuAction *menu_actions = NULL;
static int n_menu_actions = 0;
static int cap_menu_actions = 0;

static MenuAction *alloc_menu_action(TrayAudio *ta, uint32_t sink_id)
{
    if (n_menu_actions >= cap_menu_actions) {
        cap_menu_actions = cap_menu_actions ? cap_menu_actions * 2 : 16;
        menu_actions = realloc(menu_actions,
                               cap_menu_actions * sizeof(MenuAction));
    }
    MenuAction *a = &menu_actions[n_menu_actions++];
    a->ta = ta;
    a->sink_id = sink_id;
    return a;
}

static void on_select_sink(Widget w, IswPointer client_data,
                           IswPointer call_data)
{
    (void)w; (void)call_data;
    MenuAction *a = (MenuAction *)client_data;
    ta_pw_set_default_sink(a->ta, a->sink_id);
}

static void on_mute_default(Widget w, IswPointer client_data,
                            IswPointer call_data)
{
    (void)w; (void)call_data;
    TrayAudio *ta = (TrayAudio *)client_data;
    SinkInfo *def = ta_default_sink(ta);
    if (def)
        ta_pw_set_mute(ta, def->id, def->muted ? 0 : 1);
}

static void menu_popdown_cb(Widget w, IswPointer client_data,
                            IswPointer call_data)
{
    (void)client_data; (void)call_data;
    xcb_ungrab_pointer(IswDisplay(w), XCB_CURRENT_TIME);
    xcb_flush(IswDisplay(w));
}

static void menu_grab_handler(Widget w, IswPointer closure,
                              xcb_generic_event_t *event,
                              Boolean *cont)
{
    (void)closure;
    uint8_t type = event->response_type & 0x7f;

    if (type == XCB_BUTTON_PRESS) {
        xcb_button_press_event_t *ev = (xcb_button_press_event_t *)event;
        double sf = ISWScaleFactor(w);
        int pw = (int)(w->core.width * sf + 0.5);
        int ph = (int)(w->core.height * sf + 0.5);
        if (ev->event_x < 0 || ev->event_y < 0 ||
            ev->event_x >= pw || ev->event_y >= ph) {
            xcb_ungrab_pointer(IswDisplay(w), XCB_CURRENT_TIME);
            xcb_flush(IswDisplay(w));
            IswPopdown(w);
            *cont = False;
            return;
        }
    }
    *cont = True;
}

void ta_menu_init(TrayAudio *ta)
{
    Arg args[20];
    Cardinal n = 0;

    ta->menu_shell = IswCreatePopupShell("audioMenu", simpleMenuWidgetClass,
                                          ta->toplevel, args, n);
}

void ta_menu_show(TrayAudio *ta)
{
    /* Destroy and recreate */
    if (ta->menu_shell)
        IswDestroyWidget(ta->menu_shell);

    n_menu_actions = 0;

    Arg args[20];
    Cardinal n = 0;

    ta->menu_shell = IswCreatePopupShell("audioMenu", simpleMenuWidgetClass,
                                          ta->toplevel, args, n);

    if (ta->nsinks == 0) {
        n = 0;
        IswSetArg(args[n], IswNlabel, "No audio outputs"); n++;
        IswSetArg(args[n], IswNsensitive, False); n++;
        IswCreateManagedWidget("noOutputs", smeBSBObjectClass,
                              ta->menu_shell, args, n);
    } else {
        /* Section header */
        n = 0;
        IswSetArg(args[n], IswNlabel, "Output Device"); n++;
        IswSetArg(args[n], IswNsensitive, False); n++;
        IswCreateManagedWidget("hdrOutput", smeBSBObjectClass,
                              ta->menu_shell, args, n);

        for (int i = 0; i < ta->nsinks; i++) {
            SinkInfo *s = &ta->sinks[i];
            char label[280];
            if (s->is_default)
                snprintf(label, sizeof(label), "* %s", s->name);
            else
                snprintf(label, sizeof(label), "  %s", s->name);

            n = 0;
            IswSetArg(args[n], IswNlabel, label); n++;
            Widget w = IswCreateManagedWidget("sinkItem", smeBSBObjectClass,
                                             ta->menu_shell, args, n);
            MenuAction *a = alloc_menu_action(ta, s->id);
            IswAddCallback(w, IswNcallback, on_select_sink, a);
        }

        /* Separator + mute toggle */
        IswCreateManagedWidget("sep", smeLineObjectClass,
                              ta->menu_shell, NULL, 0);

        SinkInfo *def = ta_default_sink(ta);
        n = 0;
        IswSetArg(args[n], IswNlabel,
                  (def && def->muted) ? "Unmute" : "Mute"); n++;
        Widget mw = IswCreateManagedWidget("muteItem", smeBSBObjectClass,
                                           ta->menu_shell, args, n);
        IswAddCallback(mw, IswNcallback, on_mute_default, ta);
    }

    /* Re-attach to tray icon */
    if (ta->tray_icon)
        IswTrayIconSetMenu(ta->tray_icon, ta->menu_shell);

    IswPopup(ta->menu_shell, IswGrabExclusive);

    /* Grab + dismiss */
    {
        xcb_connection_t *conn = IswDisplay(ta->toplevel);
        xcb_grab_pointer(conn, 1, IswWindow(ta->menu_shell),
                         XCB_EVENT_MASK_BUTTON_PRESS |
                         XCB_EVENT_MASK_BUTTON_RELEASE,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                         XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
        xcb_flush(conn);
    }
    IswAddRawEventHandler(ta->menu_shell, 0, True,
                          menu_grab_handler, NULL);
    IswAddCallback(ta->menu_shell, IswNpopdownCallback,
                   menu_popdown_cb, NULL);

    /* Position above tray icon */
    if (ta->tray_icon) {
        xcb_connection_t *conn = IswDisplay(ta->toplevel);
        xcb_window_t icon_win = IswTrayIconGetWindow(ta->tray_icon);
        xcb_window_t root = IswScreen(ta->toplevel)->root;

        xcb_translate_coordinates_cookie_t cookie =
            xcb_translate_coordinates(conn, icon_win, root, 0, 0);
        xcb_translate_coordinates_reply_t *reply =
            xcb_translate_coordinates_reply(conn, cookie, NULL);

        if (reply) {
            double sf = ISWScaleFactor(ta->toplevel);
            int phys_mw = (int)(ta->menu_shell->core.width * sf + 0.5);
            int phys_mh = (int)(ta->menu_shell->core.height * sf + 0.5);
            int phys_bw = (int)(ta->menu_shell->core.border_width * sf + 0.5);
            int total_w = phys_mw + 2 * phys_bw;
            int total_h = phys_mh + 2 * phys_bw;

            int scr_w = IswScreen(ta->toplevel)->width_in_pixels;

            int x = reply->dst_x;
            int y = reply->dst_y - total_h;

            if (x + total_w > scr_w)
                x = scr_w - total_w;
            if (x < 0) x = 0;
            if (y < 0) y = 0;

            uint32_t vals[] = { (uint32_t)x, (uint32_t)y };
            xcb_configure_window(conn, IswWindow(ta->menu_shell),
                                 XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                                 vals);
            xcb_flush(conn);
            free(reply);
        }
    }
}

void ta_menu_cleanup(TrayAudio *ta)
{
    if (ta->menu_shell) {
        IswDestroyWidget(ta->menu_shell);
        ta->menu_shell = NULL;
    }
    free(menu_actions);
    menu_actions = NULL;
    n_menu_actions = 0;
    cap_menu_actions = 0;
}
