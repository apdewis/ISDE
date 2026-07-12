#define _POSIX_C_SOURCE 200809L
/*
 * tray-audio-popup.c — volume popup and right-click menu for the audio tray module
 *
 * Left-click: tabbed popup with Outputs / Inputs / Applications tabs, each
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

static void popup_button_handler(Widget w, IswPointer client_data,
                                 IswEvent *event, Boolean *cont)
{
    (void)w; (void)cont;
    TrayAudio *ta = (TrayAudio *)client_data;
    if (event->kind != IswButtonDown)
        return;
    panel_dismiss_popup(ta->panel);
    ta->popup_visible = 0;
}

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

    /* Persist manual volume choice for sinks/sources (not streams). */
    if (!r->is_source) {
        SinkInfo *sink = ta_find_sink(r->ta, r->node_id);
        if (sink && sink->node_name[0])
            ta_state_record_volume(r->ta, 0, sink->node_name, vol);
    } else {
        SourceInfo *source = ta_find_source(r->ta, r->node_id);
        if (source && source->node_name[0])
            ta_state_record_volume(r->ta, 1, source->node_name, vol);
    }
}

static void on_mute_clicked(Widget w, IswPointer client_data,
                            IswPointer call_data)
{
    (void)w; (void)call_data;
    VolumeRow *r = (VolumeRow *)client_data;
    if (r->ta->updating)
        return;

    int muted = 0;
    SinkInfo *sink = ta_find_sink(r->ta, r->node_id);
    SourceInfo *source = ta_find_source(r->ta, r->node_id);
    if (sink) {
        muted = sink->muted;
    } else if (source) {
        muted = source->muted;
    } else {
        StreamInfo *stream = ta_find_stream(r->ta, r->node_id);
        if (stream)
            muted = stream->muted;
    }

    int new_muted = muted ? 0 : 1;
    ta_pw_set_mute(r->ta, r->node_id, new_muted);

    /* Persist manual mute choice for sinks/sources (not streams). */
    if (!r->is_source) {
        if (sink && sink->node_name[0])
            ta_state_record_mute(r->ta, 0, sink->node_name, new_muted);
    } else {
        if (source && source->node_name[0])
            ta_state_record_mute(r->ta, 1, source->node_name, new_muted);
    }
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
        if (r->is_source) {
            ta_pw_set_default_source(r->ta, r->node_id);
            SourceInfo *source = ta_find_source(r->ta, r->node_id);
            if (source && source->node_name[0])
                ta_state_record_default_source(r->ta, source->node_name);
        } else {
            ta_pw_set_default_sink(r->ta, r->node_id);
            SinkInfo *sink = ta_find_sink(r->ta, r->node_id);
            if (sink && sink->node_name[0])
                ta_state_record_default_sink(r->ta, sink->node_name);
        }
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
    IswArgBorderWidth(&ab, 0);
    IswArgInternalWidth(&ab, 16);
    IswArgHeight(&ab, 60);
    IswArgRowPadding(&ab, 8);
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
        IswArgBorderWidth(&ab, 0);
        IswArgJustify(&ab, IswJustifyLeft);
        IswArgState(&ab, is_default ? True : False);
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
        IswCreateManagedWidget("streamLabel", labelWidgetClass,
                               top_row, ab.args, ab.count);
    }

    /* Second row: slider + mute */
    IswArgBuilderReset(&ab);
    IswArgBorderWidth(&ab, 0);
    IswArgInternalWidth(&ab, 16);
    IswArgRowPadding(&ab, 8);
    IswArgHeight(&ab, 40);
    Widget ctl_row = IswCreateManagedWidget("volCtl", listBoxRowWidgetClass,
                                            listbox, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgMinimumValue(&ab, 0);
    IswArgMaximumValue(&ab, 100);
    IswArgSliderValue(&ab, (int)(volume * 100.0f + 0.5f));
    IswArgJustify(&ab, IswJustifyLeft);
    IswArgShowValue(&ab, False);
    Widget sl = IswCreateManagedWidget("volSlider", sliderWidgetClass,
                                       ctl_row, ab.args, ab.count);
    IswAddCallback(sl, IswNvalueChanged, on_slider_changed, row);
    row->slider = sl;

    const char *icon_name = muted ? "audio-volume-muted" : volume_icon_name(volume);
    char *icon_path = isde_icon_find("status", icon_name);
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "");
    if (icon_path)
        IswArgImage(&ab, icon_path);
    IswArgMinWidth(&ab, 48);
    IswArgHeight(&ab, 24);
    
    Widget mb = IswCreateManagedWidget("volMute", commandWidgetClass,
                                       ctl_row, ab.args, ab.count);
    free(icon_path);
    IswAddCallback(mb, IswNcallback, on_mute_clicked, row);
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

/* ---------- position popup above the tray icon ---------- */

static void position_popup(TrayAudio *ta)
{
    Panel *p = ta->panel;
    Widget popup = ta->popup_shell;

    if (!popup || !ta->icon)
        return;

    double sf = ISWScaleFactor(p->toplevel);
    int log_panel_top = (int)((p->mon_y + p->mon_h) / sf + 0.5) - PANEL_HEIGHT;

    if (!IswIsRealized(popup)) {
        IswRealizeWidget(popup);
    }

    int popup_w = popup->core.width;
    int popup_h = popup->core.height;
    int popup_bw = popup->core.border_width;

    /* Get the icon's position relative to the panel shell */
    int icon_x = 0;
    Widget w = ta->icon;
    while (w && w != p->shell) {
        icon_x += w->core.x + w->core.border_width;
        w = w->core.parent;
    }
    int log_mon_x = (int)(p->mon_x / sf + 0.5);
    int log_mon_w = (int)(p->mon_w / sf + 0.5);
    int log_shell_x = p->shell->core.x;

    int icon_center = log_shell_x + icon_x + ta->icon->core.width / 2;
    int x = icon_center - (popup_w + 2 * popup_bw) / 2;
    int y = log_panel_top - popup_h - 2 * popup_bw;

    /* Clamp to monitor bounds */
    if (x + popup_w + 2 * popup_bw > log_mon_x + log_mon_w)
        x = log_mon_x + log_mon_w - popup_w - 2 * popup_bw;
    if (x < log_mon_x)
        x = log_mon_x;

    IswConfigureWidget(popup, x, y, popup_w, popup_h, popup_bw);
}

/* ---------- public popup API ---------- */

void ta_popup_init(TrayAudio *ta)
{
    ta->popup_shell = NULL;
    ta->popup_outer = NULL;
    ta->tabs = NULL;
    ta->output_page = NULL;
    ta->input_page = NULL;
    ta->app_page = NULL;
    ta->popup_visible = 0;
}

void ta_popup_show(TrayAudio *ta)
{
    Panel *p = ta->panel;
    const IsdeColorScheme *scheme = isde_theme_current();

    if (ta->popup_visible) {
        ta_popup_hide(ta);
        return;
    }

    /* Reset row allocations */
    nrows = 0;

    if (!ta->popup_shell) {
        IswArgBuilder ab = IswArgBuilderInit();

        /* Override shell for popup */
        IswArgWidth(&ab, 280);
        IswArgHeight(&ab, 400);
        IswArgBorderWidth(&ab, 0);
        ta->popup_shell = IswCreatePopupShell("audioPopup",
                                            overrideShellWidgetClass,
                                            p->toplevel, ab.args, ab.count);
        IswAddEventHandler(ta->popup_shell, IswButtonPressMask, False,
                           popup_button_handler, ta);

        /* Outer vertical FlexBox */
        IswArgBuilderReset(&ab);
        IswArgOrientation(&ab, IswOrientVertical);
        IswArgBorderWidth(&ab, 0);
        ta->popup_outer = IswCreateManagedWidget("outerBox", flexBoxWidgetClass,
                                                ta->popup_shell,
                                                ab.args, ab.count);
                                                
        /* Tabs container */
        IswArgBuilderReset(&ab);
        IswArgFlexGrow(&ab, 1);
        IswArgBorderWidth(&ab, 1);
        IswArgWidth(&ab, 280);
        IswArgTabHeight(&ab, 30);
        IswArgTabSizing(&ab, IswTabSizingFill);
        ta->tabs = IswCreateManagedWidget("tabs", tabsWidgetClass,
                                        ta->popup_outer, ab.args, ab.count);

        /* Outputs tab */
        IswArgBuilderReset(&ab);
        IswArgTabLabel(&ab, "Outputs");
        IswArgSelectionMode(&ab, IswListBoxSelectNone);
        IswArgRowSpacing(&ab, 0);
        IswArgBorderWidth(&ab, 0);
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

        /* Applications tab */
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
    }

    /* Populate the visible page */
    build_output_content(ta);

    position_popup(ta);
    IswPopup(ta->popup_shell, IswGrabNone);
    IswGrabPointer(ta->popup_shell, True,
                   IswButtonPressMask | IswButtonReleaseMask,
                   IswCursorNone, ISW_CURRENT_TIME);

    panel_show_popup(p, ta->popup_shell);
    ta->popup_visible = 1;
}

void ta_popup_hide(TrayAudio *ta)
{
    if (!ta->popup_visible)
        return;

    panel_dismiss_popup(ta->panel);
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
            const char *iname = muted ? "audio-volume-muted" : volume_icon_name(vol);
            char *vi = isde_icon_find("status", iname);
            if (vi) {
                IswArgBuilder ab = IswArgBuilderInit();
                IswArgImage(&ab, vi);
                IswSetValues(r->mute_btn, ab.args, ab.count);
                free(vi);
            }
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
        ta->popup_outer = NULL;
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
    SinkInfo *sink = ta_find_sink(a->ta, a->sink_id);
    if (sink && sink->node_name[0])
        ta_state_record_default_sink(a->ta, sink->node_name);
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

static void menu_button_handler(Widget w, IswPointer client_data,
                                IswEvent *event, Boolean *cont)
{
    (void)w; (void)cont;
    Panel *p = (Panel *)client_data;
    if (event->kind != IswButtonDown)
        return;
    panel_dismiss_popup(p);
}

/* ---------- position menu above tray icon ---------- */

static void position_menu(TrayAudio *ta)
{
    Panel *p = ta->panel;
    Widget menu = ta->menu_shell;

    if (!menu || !ta->icon)
        return;

    Widget shell = IswParent(menu);

    double sf = ISWScaleFactor(p->toplevel);
    int log_panel_top = (int)((p->mon_y + p->mon_h) / sf + 0.5) - PANEL_HEIGHT;

    if (!IswIsRealized(shell)) {
        IswRealizeWidget(shell);
    }

    int menu_w = shell->core.width;
    int menu_h = shell->core.height;
    int menu_bw = shell->core.border_width;

    /* Get the icon's position relative to the panel shell */
    int icon_x = 0;
    Widget w = ta->icon;
    while (w && w != p->shell) {
        icon_x += w->core.x + w->core.border_width;
        w = w->core.parent;
    }
    int log_mon_x = (int)(p->mon_x / sf + 0.5);
    int log_mon_w = (int)(p->mon_w / sf + 0.5);
    int log_shell_x = p->shell->core.x;

    int icon_center = log_shell_x + icon_x + ta->icon->core.width / 2;
    int x = icon_center - (menu_w + 2 * menu_bw) / 2;
    int y = log_panel_top - menu_h - 2 * menu_bw;

    /* Clamp to monitor bounds */
    if (x + menu_w + 2 * menu_bw > log_mon_x + log_mon_w)
        x = log_mon_x + log_mon_w - menu_w - 2 * menu_bw;
    if (x < log_mon_x)
        x = log_mon_x;

    IswConfigureWidget(shell, x, y, menu_w, menu_h, menu_bw);
}

void ta_menu_init(TrayAudio *ta)
{
    ta->menu_shell = NULL;
}

void ta_menu_show(TrayAudio *ta)
{
    Panel *p = ta->panel;

    /* Destroy and recreate. menu_shell holds the (windowless) SimpleMenu;
     * its popup shell is IswParent(ta->menu_shell). */
    if (ta->menu_shell)
        IswDestroyWidget(IswParent(ta->menu_shell));

    n_menu_actions = 0;

    IswArgBuilder ab = IswArgBuilderInit();

    ta->menu_shell = IswCreateMenuPopupShell("audioMenu", p->toplevel, NULL, 0);
    IswAddEventHandler(IswParent(ta->menu_shell), IswButtonPressMask, False,
                       menu_button_handler, p);

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

    position_menu(ta);
    Widget shell = IswParent(ta->menu_shell);
    IswPopup(shell, IswGrabNone);
    IswGrabPointer(shell, True,
                   IswButtonPressMask | IswButtonReleaseMask,
                   IswCursorNone, ISW_CURRENT_TIME);

    panel_show_popup(p, shell);
}

void ta_menu_cleanup(TrayAudio *ta)
{
    if (ta->menu_shell) {
        IswDestroyWidget(IswParent(ta->menu_shell));
        ta->menu_shell = NULL;
    }
    free(menu_actions);
    menu_actions = NULL;
    n_menu_actions = 0;
    cap_menu_actions = 0;
}
