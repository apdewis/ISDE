#define _POSIX_C_SOURCE 200809L
/*
 * popup.c — battery popup and right-click menu for isde-tray-battery
 */
#include "tray-battery.h"

#include <ISW/IntrinsicP.h>
#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>
#include <isde/isde-tray.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Left-click popup
 * ================================================================ */

#define POPUP_DISMISS_MASK (XCB_EVENT_MASK_BUTTON_PRESS)

static void popup_outside_handler(Widget w, IswPointer closure,
                                  xcb_generic_event_t *event,
                                  Boolean *cont)
{
    (void)cont;
    TrayBattery *tb = (TrayBattery *)closure;
    uint8_t type = event->response_type & 0x7f;

    if (type != XCB_BUTTON_PRESS) {
        return;
    }
    if (!tb->popup_visible || !tb->popup_shell) {
        return;
    }

    xcb_button_press_event_t *ev = (xcb_button_press_event_t *)event;
    double sf = ISWScaleFactor(w);
    int pw = (int)(w->core.width * sf + 0.5);
    int ph = (int)(w->core.height * sf + 0.5);

    if (ev->event_x < 0 || ev->event_y < 0 ||
        ev->event_x >= pw || ev->event_y >= ph) {
        tb_popup_hide(tb);
    }
}

static const char *profile_labels[] = {
    "Power Saver", "Balanced", "Performance"
};

static void on_profile_toggled(Widget w, IswPointer client_data,
                               IswPointer call_data)
{
    (void)call_data;
    TrayBattery *tb = (TrayBattery *)client_data;
    if (tb->updating) {
        return;
    }

    Boolean state = False;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgState(&ab, &state);
    IswGetValues(w, ab.args, ab.count);

    if (!state) {
        return;
    }

    for (int i = 0; i < 3; i++) {
        if (tb->profile_radios[i] == w) {
            isde_cpufreq_set_profile((IsdeCpuProfile)i);
            break;
        }
    }
}

static void format_time_remaining(const IsdeBattery *bat, char *buf, size_t bufsz)
{
    if (!bat || bat->power_now <= 0) {
        snprintf(buf, bufsz, "Time remaining: unknown");
        return;
    }

    long energy;
    if (bat->status == ISDE_BAT_CHARGING) {
        energy = bat->energy_full - bat->energy_now;
    } else {
        energy = bat->energy_now;
    }

    if (energy <= 0) {
        snprintf(buf, bufsz, "Time remaining: unknown");
        return;
    }

    double hours = (double)energy / (double)bat->power_now;
    int h = (int)hours;
    int m = (int)((hours - h) * 60.0);

    if (bat->status == ISDE_BAT_CHARGING) {
        snprintf(buf, bufsz, "Time to full: %d:%02d", h, m);
    } else {
        snprintf(buf, bufsz, "Time remaining: %d:%02d", h, m);
    }
}

static const char *status_text(IsdeBatteryStatus st, int on_ac)
{
    switch (st) {
    case ISDE_BAT_CHARGING:     return "Charging";
    case ISDE_BAT_DISCHARGING:  return on_ac ? "Plugged in" : "On battery";
    case ISDE_BAT_FULL:         return "Fully charged";
    case ISDE_BAT_NOT_PRESENT:  return "Battery not present";
    }
    return "Unknown";
}

void tb_popup_init(TrayBattery *tb)
{
    tb->popup_shell = NULL;
    tb->popup_form = NULL;
    tb->popup_visible = 0;
}

void tb_popup_show(TrayBattery *tb)
{
    const IsdeColorScheme *scheme = isde_theme_current();

    if (tb->popup_visible) {
        tb_popup_hide(tb);
        return;
    }

    if (tb->popup_shell) {
        IswDestroyWidget(tb->popup_shell);
        tb->popup_shell = NULL;
    }

    const IsdeBattery *bat = isde_power_get_battery(tb->power, 0);
    IswArgBuilder ab = IswArgBuilderInit();

    /* Override shell */
    IswArgWidth(&ab, 280);
    IswArgHeight(&ab, 220);
    tb->popup_shell = IswCreatePopupShell("batteryPopup",
                                          overrideShellWidgetClass,
                                          tb->toplevel, ab.args, ab.count);

    /* Form layout */
    IswArgBuilderReset(&ab);
    IswArgDefaultDistance(&ab, 8);
    IswArgBorderWidth(&ab, 0);
    tb->popup_form = IswCreateManagedWidget("popupForm", formWidgetClass,
                                             tb->popup_shell,
                                             ab.args, ab.count);

    /* Capacity label */
    char cap_text[64];
    snprintf(cap_text, sizeof(cap_text), "Battery: %d%%",
             bat ? bat->capacity : 0);
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, cap_text);
    IswArgBorderWidth(&ab, 0);
    IswArgJustify(&ab, IswJustifyLeft);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainRight);
    tb->capacity_label = IswCreateManagedWidget("capLabel", labelWidgetClass,
                                                 tb->popup_form,
                                                 ab.args, ab.count);

    /* Progress bar */
    IswArgBuilderReset(&ab);
    IswArgFromVert(&ab, tb->capacity_label);
    IswArgMinimumValue(&ab, 0);
    IswArgMaximumValue(&ab, 100);
    IswArgSliderValue(&ab, bat ? bat->capacity : 0);
    IswArgWidth(&ab, 250);
    IswArgHeight(&ab, 16);
    IswArgBorderWidth(&ab, 0);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainRight);
    tb->capacity_bar = IswCreateManagedWidget("capBar", progressBarWidgetClass,
                                               tb->popup_form,
                                               ab.args, ab.count);

    /* Time remaining */
    char time_text[64];
    format_time_remaining(bat, time_text, sizeof(time_text));
    IswArgBuilderReset(&ab);
    IswArgFromVert(&ab, tb->capacity_bar);
    IswArgLabel(&ab, time_text);
    IswArgBorderWidth(&ab, 0);
    IswArgJustify(&ab, IswJustifyLeft);
    if (scheme) { IswArgForeground(&ab, scheme->fg_dim); }
    if (tb->small_font) { IswArgFont(&ab, tb->small_font); }
    IswArgLeft(&ab, IswChainLeft);
    tb->time_label = IswCreateManagedWidget("timeLabel", labelWidgetClass,
                                             tb->popup_form,
                                             ab.args, ab.count);

    /* AC/charging status */
    const char *st = status_text(bat ? bat->status : ISDE_BAT_NOT_PRESENT,
                                 isde_power_on_ac(tb->power));
    IswArgBuilderReset(&ab);
    IswArgFromVert(&ab, tb->time_label);
    IswArgLabel(&ab, st);
    IswArgBorderWidth(&ab, 0);
    IswArgJustify(&ab, IswJustifyLeft);
    if (scheme) { IswArgForeground(&ab, scheme->fg_dim); }
    if (tb->small_font) { IswArgFont(&ab, tb->small_font); }
    IswArgLeft(&ab, IswChainLeft);
    tb->status_label = IswCreateManagedWidget("statusLabel", labelWidgetClass,
                                               tb->popup_form,
                                               ab.args, ab.count);

    /* Performance profile radio buttons */
    if (isde_cpufreq_available()) {
        IsdeCpuProfile current = isde_cpufreq_get_profile();
        Widget prev_radio = NULL;
        Widget above = tb->status_label;

        /* Section label */
        IswArgBuilderReset(&ab);
        IswArgFromVert(&ab, above);
        IswArgVertDistance(&ab, 16);
        IswArgLabel(&ab, "Performance");
        IswArgBorderWidth(&ab, 0);
        IswArgJustify(&ab, IswJustifyLeft);
        IswArgLeft(&ab, IswChainLeft);
        Widget section_lbl = IswCreateManagedWidget("sectionHd", labelWidgetClass,
                                                     tb->popup_form,
                                                     ab.args, ab.count);
        above = section_lbl;

        for (int i = 0; i < 3; i++) {
            IswArgBuilderReset(&ab);
            IswArgFromVert(&ab, above);
            IswArgLabel(&ab, profile_labels[i]);
            IswArgBorderWidth(&ab, 0);
            IswArgState(&ab, (int)current == i ? True : False);
            IswArgJustify(&ab, IswJustifyLeft);
            IswArgLeft(&ab, IswChainLeft);
            if (prev_radio) {
                IswArgRadioGroup(&ab, prev_radio);
            }
            tb->profile_radios[i] =
                IswCreateManagedWidget("profileRadio", toggleWidgetClass,
                                       tb->popup_form, ab.args, ab.count);
            IswAddCallback(tb->profile_radios[i], IswNcallback,
                          on_profile_toggled, tb);
            prev_radio = tb->profile_radios[i];
            above = tb->profile_radios[i];
        }
    }

    IswRealizeWidget(tb->popup_shell);
    isde_tray_position_popup(tb->toplevel, tb->tray_icon, tb->popup_shell);
    IswPopup(tb->popup_shell, IswGrabNone);

    /* Pointer grab for click-outside dismiss */
    {
        xcb_connection_t *conn = IswDisplay(tb->toplevel);
        xcb_grab_pointer(conn, True, IswWindow(tb->popup_shell),
                         XCB_EVENT_MASK_BUTTON_PRESS |
                         XCB_EVENT_MASK_BUTTON_RELEASE,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                         XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
        xcb_flush(conn);
    }

    IswAddEventHandler(tb->popup_shell, POPUP_DISMISS_MASK, False,
                       popup_outside_handler, tb);
    tb->popup_visible = 1;
}

void tb_popup_hide(TrayBattery *tb)
{
    if (!tb->popup_visible) {
        return;
    }

    if (tb->popup_shell) {
        IswRemoveEventHandler(tb->popup_shell, POPUP_DISMISS_MASK, False,
                              popup_outside_handler, tb);
    }

    xcb_ungrab_pointer(IswDisplay(tb->toplevel), XCB_CURRENT_TIME);
    xcb_flush(IswDisplay(tb->toplevel));

    if (tb->popup_shell) {
        IswPopdown(tb->popup_shell);
    }

    tb->popup_visible = 0;
}

void tb_popup_update(TrayBattery *tb)
{
    if (!tb->popup_visible) {
        return;
    }

    tb->updating = 1;

    const IsdeBattery *bat = isde_power_get_battery(tb->power, 0);

    /* Update capacity label */
    if (tb->capacity_label) {
        char cap_text[64];
        snprintf(cap_text, sizeof(cap_text), "Battery: %d%%",
                 bat ? bat->capacity : 0);
        IswArgBuilder ab = IswArgBuilderInit();
        IswArgLabel(&ab, cap_text);
        IswSetValues(tb->capacity_label, ab.args, ab.count);
    }

    /* Update progress bar */
    if (tb->capacity_bar) {
        IswArgBuilder ab = IswArgBuilderInit();
        IswArgSliderValue(&ab, bat ? bat->capacity : 0);
        IswSetValues(tb->capacity_bar, ab.args, ab.count);
    }

    /* Update time label */
    if (tb->time_label) {
        char time_text[64];
        format_time_remaining(bat, time_text, sizeof(time_text));
        IswArgBuilder ab = IswArgBuilderInit();
        IswArgLabel(&ab, time_text);
        IswSetValues(tb->time_label, ab.args, ab.count);
    }

    /* Update status label */
    if (tb->status_label) {
        const char *st = status_text(bat ? bat->status : ISDE_BAT_NOT_PRESENT,
                                     isde_power_on_ac(tb->power));
        IswArgBuilder ab = IswArgBuilderInit();
        IswArgLabel(&ab, st);
        IswSetValues(tb->status_label, ab.args, ab.count);
    }

    /* Update profile radios */
    if (isde_cpufreq_available() && tb->profile_radios[0]) {
        IsdeCpuProfile current = isde_cpufreq_get_profile();
        for (int i = 0; i < 3; i++) {
            if (tb->profile_radios[i]) {
                IswArgBuilder ab = IswArgBuilderInit();
                IswArgState(&ab, (int)current == i ? True : False);
                IswSetValues(tb->profile_radios[i], ab.args, ab.count);
            }
        }
    }

    tb->updating = 0;
}

void tb_popup_cleanup(TrayBattery *tb)
{
    if (tb->popup_shell) {
        IswDestroyWidget(tb->popup_shell);
        tb->popup_shell = NULL;
        tb->popup_form = NULL;
    }
    tb->capacity_label = NULL;
    tb->capacity_bar = NULL;
    tb->time_label = NULL;
    tb->status_label = NULL;
    memset(tb->profile_radios, 0, sizeof(tb->profile_radios));
    tb->popup_visible = 0;
}

/* ================================================================
 * Right-click menu (profile shortcuts)
 * ================================================================ */

static void on_menu_profile(Widget w, IswPointer client_data,
                            IswPointer call_data)
{
    (void)w; (void)call_data;
    IsdeCpuProfile profile = (IsdeCpuProfile)(intptr_t)client_data;
    isde_cpufreq_set_profile(profile);
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

void tb_menu_init(TrayBattery *tb)
{
    tb->menu_shell = IswCreatePopupShell("batteryMenu", simpleMenuWidgetClass,
                                          tb->toplevel, NULL, 0);
}

void tb_menu_show(TrayBattery *tb)
{
    if (tb->menu_shell) {
        IswDestroyWidget(tb->menu_shell);
    }

    IswArgBuilder ab = IswArgBuilderInit();
    tb->menu_shell = IswCreatePopupShell("batteryMenu", simpleMenuWidgetClass,
                                          tb->toplevel, NULL, 0);

    /* Battery info header */
    const IsdeBattery *bat = isde_power_get_battery(tb->power, 0);
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "Battery: %d%%", bat ? bat->capacity : 0);
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, hdr);
    IswArgSensitive(&ab, False);
    IswCreateManagedWidget("hdrBat", smeBSBObjectClass,
                          tb->menu_shell, ab.args, ab.count);

    IswCreateManagedWidget("sep", smeLineObjectClass,
                          tb->menu_shell, NULL, 0);

    /* Profile shortcuts */
    if (isde_cpufreq_available()) {
        IsdeCpuProfile current = isde_cpufreq_get_profile();
        for (int i = 0; i < 3; i++) {
            char label[64];
            if ((int)current == i) {
                snprintf(label, sizeof(label), "* %s", profile_labels[i]);
            } else {
                snprintf(label, sizeof(label), "  %s", profile_labels[i]);
            }
            IswArgBuilderReset(&ab);
            IswArgLabel(&ab, label);
            Widget w = IswCreateManagedWidget("profileItem", smeBSBObjectClass,
                                             tb->menu_shell, ab.args, ab.count);
            IswAddCallback(w, IswNcallback, on_menu_profile,
                          (IswPointer)(intptr_t)i);
        }
    }

    if (tb->tray_icon) {
        IswTrayIconSetMenu(tb->tray_icon, tb->menu_shell);
    }

    IswPopup(tb->menu_shell, IswGrabExclusive);

    /* Grab + dismiss */
    {
        xcb_connection_t *conn = IswDisplay(tb->toplevel);
        xcb_grab_pointer(conn, 1, IswWindow(tb->menu_shell),
                         XCB_EVENT_MASK_BUTTON_PRESS |
                         XCB_EVENT_MASK_BUTTON_RELEASE,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                         XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
        xcb_flush(conn);
    }
    IswAddRawEventHandler(tb->menu_shell, 0, True,
                          menu_grab_handler, NULL);
    IswAddCallback(tb->menu_shell, IswNpopdownCallback,
                   menu_popdown_cb, NULL);

    /* Position above tray icon */
    if (tb->tray_icon) {
        xcb_connection_t *conn = IswDisplay(tb->toplevel);
        xcb_window_t icon_win = IswTrayIconGetWindow(tb->tray_icon);
        xcb_window_t root = IswScreen(tb->toplevel)->root;

        xcb_translate_coordinates_cookie_t cookie =
            xcb_translate_coordinates(conn, icon_win, root, 0, 0);
        xcb_translate_coordinates_reply_t *reply =
            xcb_translate_coordinates_reply(conn, cookie, NULL);

        if (reply) {
            double sf = ISWScaleFactor(tb->toplevel);
            int phys_mw = (int)(tb->menu_shell->core.width * sf + 0.5);
            int phys_mh = (int)(tb->menu_shell->core.height * sf + 0.5);
            int phys_bw = (int)(tb->menu_shell->core.border_width * sf + 0.5);
            int total_w = phys_mw + 2 * phys_bw;
            int total_h = phys_mh + 2 * phys_bw;

            int scr_w = IswScreen(tb->toplevel)->width_in_pixels;

            int x = reply->dst_x;
            int y = reply->dst_y - total_h;

            if (x + total_w > scr_w) {
                x = scr_w - total_w;
            }
            if (x < 0) { x = 0; }
            if (y < 0) { y = 0; }

            uint32_t vals[] = { (uint32_t)x, (uint32_t)y };
            xcb_configure_window(conn, IswWindow(tb->menu_shell),
                                 XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                                 vals);
            xcb_flush(conn);
            free(reply);
        }
    }
}

void tb_menu_cleanup(TrayBattery *tb)
{
    if (tb->menu_shell) {
        IswDestroyWidget(tb->menu_shell);
        tb->menu_shell = NULL;
    }
}
