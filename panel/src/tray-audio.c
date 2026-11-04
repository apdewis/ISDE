#define _POSIX_C_SOURCE 200809L
/*
 * tray-audio.c — audio tray module for isde-panel (PipeWire)
 */
#include "tray-audio.h"

#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>
#include <ISW/ScrollWheel.h>
#include <ISW/IntrinsicP.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- icon loading ---------- */

static const char *icon_name_for_state(int state)
{
    switch (state) {
    case 0:  return "audio-volume-muted";
    case 1:  return "audio-volume-low";
    case 2:  return "audio-volume-medium";
    case 3:  return "audio-volume-high";
    default: return "audio-volume-muted";
    }
}

static int compute_icon_state(TrayAudio *ta)
{
    SinkInfo *def = ta_default_sink(ta);
    if (!def || def->muted || def->volume <= 0.0f)
        return 0;
    if (def->volume <= 0.33f)
        return 1;
    if (def->volume <= 0.66f)
        return 2;
    return 3;
}

static void load_icon(TrayAudio *ta, int state)
{
    if (!ta->icon)
        return;

    char *icon_path = isde_icon_find("status", icon_name_for_state(state));
    if (!icon_path) {
        fprintf(stderr, "isde-panel: tray-audio: cannot find icon %s\n",
                icon_name_for_state(state));
        return;
    }

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgImage(&ab, icon_path);
    IswArgLabel(&ab, "");
    IswSetValues(ta->icon, ab.args, ab.count);

    free(icon_path);
}

void tn_audio_update_icon(TrayAudio *ta)
{
    int new_state = compute_icon_state(ta);
    if (new_state != ta->icon_state) {
        ta->icon_state = new_state;
        load_icon(ta, new_state);
    }
}

void tn_audio_reload_theme(TrayAudio *ta)
{
    load_icon(ta, ta->icon_state);
}

/* ---------- click callback ---------- */

static void on_icon_click(Widget w, IswPointer client_data,
                          IswPointer call_data)
{
    (void)w; (void)call_data;
    TrayAudio *ta = (TrayAudio *)client_data;
    ta_popup_show(ta);
}

/* ---------- event handler for right-click ---------- */

static void on_icon_event(Widget w, IswPointer client_data,
                          IswEvent *event, Boolean *cont)
{
    (void)w;
    TrayAudio *ta = (TrayAudio *)client_data;

    if (event->kind != IswButtonDown)
        return;

    if (event->button.button == IswButtonRight) {
        ta_menu_show(ta);
        *cont = False;
    }
}

/* ---------- scroll wheel volume ---------- */

/* Scroll is not a button event and is not delivered to event handlers or
 * translations once ISW's scroll dispatcher is installed (any Viewport or
 * Scrollbar in the panel installs it; it routes only to scrollable
 * containers).  So we sit in front of it: scrolls over the volume icon
 * adjust volume, everything else is chained to ISW's dispatcher. */

static TrayAudio           *scroll_ta;
static IswEventDispatchProc prev_scroll_dispatch;
static float                scroll_accum;

static Boolean pointer_over_icon(Widget icon, IswEvent *event)
{
    Widget root = (Widget)(void *)event->any.target;
    Widget a;
    int ix = 0, iy = 0;

    if (!root || !IswIsWidget(root) || !IswIsRealized(icon))
        return False;

    /* Event must come from the icon's shell */
    while (root && !IswIsShell(root))
        root = IswParent(root);
    for (a = icon; a && !IswIsShell(a); a = IswParent(a)) {
        ix += a->core.x + (int)a->core.border_width;
        iy += a->core.y + (int)a->core.border_width;
    }
    if (root != a)
        return False;

    return event->scroll.shell_x >= ix &&
           event->scroll.shell_x < ix + (int)icon->core.width &&
           event->scroll.shell_y >= iy &&
           event->scroll.shell_y < iy + (int)icon->core.height;
}

static Boolean scroll_dispatch(IswEvent *event, IswDisplay dpy)
{
    TrayAudio *ta = scroll_ta;
    int steps;

    if (event->kind != IswScroll || !ta || !ta->icon ||
        !pointer_over_icon(ta->icon, event))
        return prev_scroll_dispatch ? prev_scroll_dispatch(event, dpy) : False;

    /* Negative = up = louder.  Smooth deltas count one step per
     * wheel-notch-equivalent of pixels. */
    if (event->scroll.smooth) {
        scroll_accum += event->scroll.delta_y;
        steps = (int)(scroll_accum / ISW_SCROLL_WHEEL_DEFAULT_INCREMENT);
        scroll_accum -= (float)steps * ISW_SCROLL_WHEEL_DEFAULT_INCREMENT;
    } else {
        scroll_accum = 0;
        steps = event->scroll.discrete_y;
    }
    if (steps == 0)
        return True;

    SinkInfo *def = ta_default_sink(ta);
    if (def) {
        float vol = def->volume - 0.05f * (float)steps;
        if (vol < 0.0f) vol = 0.0f;
        if (vol > 1.0f) vol = 1.0f;
        def->volume = vol;
        ta_pw_set_volume(ta, def->id, vol);
        if (def->node_name[0])
            ta_state_record_volume(ta, 0, def->node_name, vol);
        tn_audio_update_icon(ta);
    }
    return True;
}

/* ---------- deferred initial icon load ---------- */

static void deferred_icon_load(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayAudio *ta = (TrayAudio *)client_data;
    ta->icon_state = compute_icon_state(ta);
    load_icon(ta, ta->icon_state);
}

/* ---------- public API ---------- */

void tn_audio_init(Panel *p)
{
    TrayAudio *ta = calloc(1, sizeof(TrayAudio));
    p->tray_audio = ta;
    ta->panel = p;

    ta->icon = panel_tray_add_icon(p, "trayBtn", commandWidgetClass);
    IswAddCallback(ta->icon, IswNcallback, on_icon_click, ta);
    IswAddEventHandler(ta->icon, IswButtonPressMask, False,
                       on_icon_event, ta);

    /* Install ISW's scroll dispatcher now so ours is guaranteed to sit in
     * front of it (it never chains, and is otherwise installed lazily by
     * the first Scrollbar). */
    IswDisplay dpy = IswDisplayOf(ta->icon);
    ISWScrollWheelInit(dpy);
    scroll_ta = ta;
    prev_scroll_dispatch = IswSetEventDispatcher(dpy, IswScroll,
                                                 scroll_dispatch);

    /* Initialize popup and menu */
    ta_popup_init(ta);
    ta_menu_init(ta);

    /* Load persisted manual choices before PipeWire binds nodes */
    ta_state_load(ta);

    /* Initialize PipeWire */
    if (ta_pw_init(ta) != 0) {
        fprintf(stderr, "isde-panel: tray-audio: PipeWire unavailable\n");
    }

    /* Load the system small font (from [fonts] config, default 9pt) */
    const char *fam = "Sans";
    int sz = 9;
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf,
                                            sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *fonts = isde_config_table(root, "fonts");
        if (fonts) {
            fam = isde_config_string(fonts, "small_family", fam);
            int csz = (int)isde_config_int(fonts, "small_size", 0);
            if (csz > 0) sz = csz;
        }
    }
    char spec[128];
    snprintf(spec, sizeof(spec), "%s-%d", fam, sz);
    ta->small_font = isde_resolve_font(p->toplevel, spec);
    if (cfg) isde_config_free(cfg);

    ta->icon_state = -1;  /* Force icon load on first update */

    IswAppAddTimeOut(p->app, 100, deferred_icon_load, ta);
}

void tn_audio_cleanup(Panel *p)
{
    TrayAudio *ta = p->tray_audio;
    if (!ta)
        return;

    scroll_ta = NULL;
    ta_popup_cleanup(ta);
    ta_menu_cleanup(ta);
    ta_pw_cleanup(ta);

    ta_state_save_now(ta);
    ta_state_cleanup(ta);

    if (ta->icon) {
        panel_tray_remove_icon(p, ta->icon);
        ta->icon = NULL;
    }

    free(ta);
    p->tray_audio = NULL;
}
