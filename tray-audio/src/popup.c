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
#include <ISW/IswArgMacros.h>

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
    Widget      radio;
    int         is_source;
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
    r->radio = NULL;
    r->is_source = 0;
    return r;
}

static const char *volume_icon_name(float vol)
{
    if (vol <= 0.0f)  return "audio-volume-muted";
    if (vol <= 0.33f) return "audio-volume-low";
    if (vol <= 0.66f) return "audio-volume-medium";
    return "audio-volume-high";
}

/* ---------- callbacks ---------- */

static void on_slider_changed(Widget w, IswPointer client_data,
                              IswPointer call_data)
{
    (void)w;
    VolumeRow *r = (VolumeRow *)client_data;
    if (r->ta->updating)
        return;
    IswSliderCallbackData *cd = (IswSliderCallbackData *)call_data;

    float vol = (float)cd->value / 100.0f;
    ta_pw_set_volume(r->ta, r->node_id, vol);
}

static void on_mute_toggled(Widget w, IswPointer client_data,
                            IswPointer call_data)
{
    (void)call_data;
    VolumeRow *r = (VolumeRow *)client_data;
    if (r->ta->updating)
        return;

    Boolean state = False;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgState(&ab, &state);
    IswGetValues(w, ab.args, ab.count);

    ta_pw_set_mute(r->ta, r->node_id, state ? 1 : 0);
}

static void on_radio_toggled(Widget w, IswPointer client_data,
                             IswPointer call_data)
{
    (void)call_data;
    VolumeRow *r = (VolumeRow *)client_data;
    if (r->ta->updating)
        return;

    Boolean state = False;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgState(&ab, &state);
    IswGetValues(w, ab.args, ab.count);

    if (state) {
        if (r->is_source)
            ta_pw_set_default_source(r->ta, r->node_id);
        else
            ta_pw_set_default_sink(r->ta, r->node_id);
    }
}

/* ---------- build a single volume row ---------- */

static void build_volume_row(TrayAudio *ta, Widget listbox,
                             const char *label_text,
                             uint32_t node_id,
                             float volume, int muted,
                             Widget radio_peer, int is_default,
                             Widget *radio_out)
{
    IswArgBuilder ab = IswArgBuilderInit();
    VolumeRow *row = alloc_row(ta, node_id);

    /* First row: radio + label */
    IswArgBuilderReset(&ab);
    IswArgRowPadding(&ab, 0);
    IswArgBorderWidth(&ab, 0);
    IswArgHeight(&ab, 60);
    Widget top_row = IswCreateManagedWidget("volName", listBoxRowWidgetClass,
                                            listbox, ab.args, ab.count);

    char wrapped[512];
    strncpy(wrapped, label_text, sizeof(wrapped) - 1);
    wrapped[sizeof(wrapped) - 1] = '\0';
    if (strlen(wrapped) > 25) {
        for (int k = 25; wrapped[k]; k++) {
            if (wrapped[k] == ' ') {
                wrapped[k] = '\n';
                break;
            }
        }
    }

    if (radio_out) {
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, wrapped);
        IswArgRowPadding(&ab, 0);
        IswArgBorderWidth(&ab, 0);
        IswArgInternalWidth(&ab, 4);
        IswArgJustify(&ab, IswJustifyLeft);
        IswArgState(&ab, is_default ? True : False);
        IswArgWidth(&ab, 300);
        IswArgHeight(&ab, 60);
        if (radio_peer)
            IswArgRadioGroup(&ab, radio_peer);
        Widget rb = IswCreateManagedWidget("outRadio", toggleWidgetClass,
                                           top_row, ab.args, ab.count);
        IswAddCallback(rb, IswNcallback, on_radio_toggled, row);
        row->radio = rb;
        *radio_out = rb;
    } else {
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, wrapped);
        IswArgBorderWidth(&ab, 0);
        IswArgJustify(&ab, IswJustifyLeft);
        IswArgWidth(&ab, 300);
        IswArgHeight(&ab, 60);
        IswCreateManagedWidget("streamLabel", labelWidgetClass,
                               top_row, ab.args, ab.count);
    }

    /* Second row: slider + mute */
    IswArgBuilderReset(&ab);
    IswArgRowPadding(&ab, 0);
    IswArgBorderWidth(&ab, 0);
    IswArgHeight(&ab, 40);
    Widget ctl_row = IswCreateManagedWidget("volCtl", listBoxRowWidgetClass,
                                            listbox, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgMinimumValue(&ab, 0);
    IswArgMaximumValue(&ab, 100);
    IswArgSliderValue(&ab, (int)(volume * 100.0f + 0.5f));
    IswArgShowValue(&ab, False);
    Widget sl = IswCreateManagedWidget("volSlider", sliderWidgetClass,
                                       ctl_row, ab.args, ab.count);
    IswAddCallback(sl, IswNvalueChanged, on_slider_changed, row);
    row->slider = sl;

    char *muted_icon = isde_icon_find("status", "audio-volume-muted");
    char *vol_icon = isde_icon_find("status", volume_icon_name(volume));
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "");
    if (muted_icon)
        IswArgImageOn(&ab, muted_icon);
    if (vol_icon)
        IswArgImageOff(&ab, vol_icon);
    IswArgState(&ab, muted ? True : False);
    IswArgWidth(&ab, 20);
    IswArgHeight(&ab, 20);
    IswArgJustify(&ab, IswJustifyLeft);
    Widget mb = IswCreateManagedWidget("volMute", toggleButtonWidgetClass,
                                       ctl_row, ab.args, ab.count);
    free(muted_icon);
    free(vol_icon);
    IswAddCallback(mb, IswNcallback, on_mute_toggled, row);
    row->mute_btn = mb;
}

static void clear_children(Widget w)
{
    WidgetList children;
    Cardinal num;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgBuilderAdd(&ab, IswNchildren, (IswArgVal)&children);
    IswArgBuilderAdd(&ab, IswNnumChildren, (IswArgVal)&num);
    IswGetValues(w, ab.args, ab.count);

    /* Destroy in reverse to avoid index shifting */
    for (int i = (int)num - 1; i >= 0; i--)
        IswDestroyWidget(children[i]);
}

/* ---------- build tab pages ---------- */

static void build_output_content(TrayAudio *ta)
{
    clear_children(ta->output_page);

    if (ta->nsinks == 0) {
        IswArgBuilder ab = IswArgBuilderInit();
        const IsdeColorScheme *scheme = isde_theme_current();
        IswArgLabel(&ab, "No audio outputs");
        IswArgBorderWidth(&ab, 0);
        IswArgSelectable(&ab, False);
        if (scheme)
            IswArgForeground(&ab, scheme->fg_dim);
        if (ta->small_font)
            IswArgFont(&ab, ta->small_font);
        IswCreateManagedWidget("noOutputs", labelWidgetClass,
                              ta->output_page, ab.args, ab.count);
        return;
    }

    Widget first_radio = NULL;
    for (int i = 0; i < ta->nsinks; i++) {
        SinkInfo *s = &ta->sinks[i];
        Widget radio = NULL;

        build_volume_row(ta, ta->output_page,
                         s->name, s->id, s->volume, s->muted,
                         first_radio, s->is_default, &radio);
        if (!first_radio)
            first_radio = radio;
    }
}

static void build_input_content(TrayAudio *ta)
{
    clear_children(ta->input_page);

    if (ta->nsources == 0) {
        IswArgBuilder ab = IswArgBuilderInit();
        const IsdeColorScheme *scheme = isde_theme_current();
        IswArgLabel(&ab, "No audio inputs");
        IswArgBorderWidth(&ab, 0);
        IswArgSelectable(&ab, False);
        if (scheme)
            IswArgForeground(&ab, scheme->fg_dim);
        if (ta->small_font)
            IswArgFont(&ab, ta->small_font);
        IswCreateManagedWidget("noInputs", labelWidgetClass,
                              ta->input_page, ab.args, ab.count);
        return;
    }

    Widget first_radio = NULL;
    for (int i = 0; i < ta->nsources; i++) {
        SourceInfo *s = &ta->sources[i];
        Widget radio = NULL;

        build_volume_row(ta, ta->input_page,
                         s->name, s->id, s->volume, s->muted,
                         first_radio, s->is_default, &radio);
        if (!first_radio)
            first_radio = radio;

        /* Tag rows just created as source rows */
        for (int j = nrows - 1; j >= 0; j--) {
            if (rows[j].node_id == s->id) {
                rows[j].is_source = 1;
                break;
            }
        }
    }
}

static void build_app_content(TrayAudio *ta)
{
    clear_children(ta->app_page);

    if (ta->nstreams == 0) {
        IswArgBuilder ab = IswArgBuilderInit();
        const IsdeColorScheme *scheme = isde_theme_current();
        IswArgLabel(&ab, "No applications playing");
        IswArgBorderWidth(&ab, 0);
        IswArgSelectable(&ab, False);
        if (scheme)
            IswArgForeground(&ab, scheme->fg_dim);
        if (ta->small_font)
            IswArgFont(&ab, ta->small_font);
        IswCreateManagedWidget("noStreams", labelWidgetClass,
                              ta->app_page, ab.args, ab.count);
        return;
    }

    for (int i = 0; i < ta->nstreams; i++) {
        StreamInfo *s = &ta->streams[i];
        build_volume_row(ta, ta->app_page,
                         s->name, s->id, s->volume, s->muted,
                         NULL, 0, NULL);
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
    else if (tcs->child == ta->input_page)
        build_input_content(ta);
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
    int icon_x = (int)(reply->dst_x / sf);
    int icon_y = (int)(reply->dst_y / sf);
    free(reply);

    Dimension w = ta->popup_shell->core.width;
    Dimension h = ta->popup_shell->core.height;
    Dimension bw = ta->popup_shell->core.border_width;
    int total_w = (int)(w + 2 * bw);
    int total_h = (int)(h + 2 * bw);
    int scr_w = (int)(IswScreen(ta->toplevel)->width_in_pixels / sf);

    int x = icon_x;
    int y = icon_y - total_h;

    if (x + total_w > scr_w)
        x = scr_w - total_w;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    IswConfigureWidget(ta->popup_shell, x, y, w, h, bw);
}

/* ---------- public popup API ---------- */

void ta_popup_init(TrayAudio *ta)
{
    /* Shell created fresh each time in ta_popup_show */
    ta->popup_shell = NULL;
    ta->tabs = NULL;
    ta->output_page = NULL;
    ta->input_page = NULL;
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

    IswArgBuilder ab = IswArgBuilderInit();

    /* Override shell for popup — border via theme resources */
    IswArgWidth(&ab, 350);
    IswArgHeight(&ab, 250);
    ta->popup_shell = IswCreatePopupShell("audioPopup",
                                          overrideShellWidgetClass,
                                          ta->toplevel, ab.args, ab.count);

    /* Tabs container */
    ta->tabs = IswCreateManagedWidget("tabs", tabsWidgetClass,
                                      ta->popup_shell, NULL, 0);

    /* Outputs tab */
    IswArgBuilderReset(&ab);
    IswArgTabLabel(&ab, "Outputs");
    IswArgSelectionMode(&ab, IswListBoxSelectNone);
    IswArgBorderWidth(&ab, 0);
    IswArgRowSpacing(&ab, 0);
    if (ta->small_font)
        IswArgFont(&ab, ta->small_font);
    ta->output_page = IswCreateManagedWidget("outputPage",
                                             listBoxWidgetClass,
                                             ta->tabs, ab.args, ab.count);

    /* Inputs tab */
    IswArgBuilderReset(&ab);
    IswArgTabLabel(&ab, "Inputs");
    IswArgSelectionMode(&ab, IswListBoxSelectNone);
    IswArgBorderWidth(&ab, 0);
    IswArgRowSpacing(&ab, 0);
    if (ta->small_font)
        IswArgFont(&ab, ta->small_font);
    ta->input_page = IswCreateManagedWidget("inputPage",
                                            listBoxWidgetClass,
                                            ta->tabs, ab.args, ab.count);

    /* Applications tab — created empty, populated on tab switch */
    IswArgBuilderReset(&ab);
    IswArgTabLabel(&ab, "Applications");
    IswArgSelectionMode(&ab, IswListBoxSelectNone);
    IswArgBorderWidth(&ab, 0);
    IswArgRowSpacing(&ab, 0);
    if (ta->small_font)
        IswArgFont(&ab, ta->small_font);
    ta->app_page = IswCreateManagedWidget("appPage",
                                          listBoxWidgetClass,
                                          ta->tabs, ab.args, ab.count);

    /* Show the outputs tab by default */
    IswTabsSetTop(ta->tabs, ta->output_page);
    IswAddCallback(ta->tabs, IswNtabCallback, on_tab_changed, ta);

    /* Pop up without Xt grab — we handle dismissal ourselves,
     * matching the MenuBar.c pattern. */
    IswRealizeWidget(ta->popup_shell);
    position_popup(ta);
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
    ta->updating = 1;
    for (int i = 0; i < nrows; i++) {
        VolumeRow *r = &rows[i];

        float vol = 0.0f;
        int muted = 0;

        SinkInfo *sink = ta_find_sink(r->ta, r->node_id);
        SourceInfo *source = NULL;
        if (sink) {
            vol = sink->volume;
            muted = sink->muted;
        } else {
            source = ta_find_source(r->ta, r->node_id);
            if (source) {
                vol = source->volume;
                muted = source->muted;
            } else {
                StreamInfo *stream = ta_find_stream(r->ta, r->node_id);
                if (stream) {
                    vol = stream->volume;
                    muted = stream->muted;
                }
            }
        }

        if (r->slider)
            IswSliderSetValue(r->slider, (int)(vol * 100.0f + 0.5f));

        if (r->mute_btn) {
            char *vi = isde_icon_find("status", volume_icon_name(vol));
            IswArgBuilder ab = IswArgBuilderInit();
            IswArgState(&ab, muted ? True : False);
            if (vi)
                IswArgImageOff(&ab, vi);
            IswSetValues(r->mute_btn, ab.args, ab.count);
            free(vi);
        }

        if (r->radio) {
            if (sink) {
                IswArgBuilder ab = IswArgBuilderInit();
                IswArgState(&ab, sink->is_default ? True : False);
                IswSetValues(r->radio, ab.args, ab.count);
            } else if (source) {
                IswArgBuilder ab = IswArgBuilderInit();
                IswArgState(&ab, source->is_default ? True : False);
                IswSetValues(r->radio, ab.args, ab.count);
            }
        }
    }
    ta->updating = 0;
}

void ta_popup_cleanup(TrayAudio *ta)
{
    if (ta->popup_shell) {
        IswDestroyWidget(ta->popup_shell);
        ta->popup_shell = NULL;
    }
    ta->tabs = NULL;
    ta->output_page = NULL;
    ta->input_page = NULL;
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
    ta->menu_shell = IswCreatePopupShell("audioMenu", simpleMenuWidgetClass,
                                          ta->toplevel, NULL, 0);
}

void ta_menu_show(TrayAudio *ta)
{
    /* Destroy and recreate */
    if (ta->menu_shell)
        IswDestroyWidget(ta->menu_shell);

    n_menu_actions = 0;

    IswArgBuilder ab = IswArgBuilderInit();

    ta->menu_shell = IswCreatePopupShell("audioMenu", simpleMenuWidgetClass,
                                          ta->toplevel, NULL, 0);

    if (ta->nsinks == 0) {
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, "No audio outputs");
        IswArgSensitive(&ab, False);
        IswCreateManagedWidget("noOutputs", smeBSBObjectClass,
                              ta->menu_shell, ab.args, ab.count);
    } else {
        /* Section header */
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, "Output Device");
        IswArgSensitive(&ab, False);
        IswCreateManagedWidget("hdrOutput", smeBSBObjectClass,
                              ta->menu_shell, ab.args, ab.count);

        for (int i = 0; i < ta->nsinks; i++) {
            SinkInfo *s = &ta->sinks[i];
            char label[280];
            if (s->is_default)
                snprintf(label, sizeof(label), "* %s", s->name);
            else
                snprintf(label, sizeof(label), "  %s", s->name);

            IswArgBuilderReset(&ab);
            IswArgLabel(&ab, label);
            Widget w = IswCreateManagedWidget("sinkItem", smeBSBObjectClass,
                                             ta->menu_shell, ab.args, ab.count);
            MenuAction *a = alloc_menu_action(ta, s->id);
            IswAddCallback(w, IswNcallback, on_select_sink, a);
        }

        /* Separator + mute toggle */
        IswCreateManagedWidget("sep", smeLineObjectClass,
                              ta->menu_shell, NULL, 0);

        SinkInfo *def = ta_default_sink(ta);
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, (def && def->muted) ? "Unmute" : "Mute");
        Widget mw = IswCreateManagedWidget("muteItem", smeBSBObjectClass,
                                           ta->menu_shell, ab.args, ab.count);
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
