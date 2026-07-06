#define _POSIX_C_SOURCE 200809L
/*
 * tray-battery-popup.c — popup and right-click menu for battery tray module
 */
#include "tray-battery.h"

#include <ISW/IntrinsicP.h>
#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ================================================================
 * Left-click popup
 * ================================================================ */

static void popup_button_handler(Widget w, IswPointer client_data,
                                 IswEvent *event, Boolean *cont)
{
    (void)w; (void)cont;
    TrayBattery *tb = (TrayBattery *)client_data;
    if (event->kind != IswButtonDown)
        return;
    panel_dismiss_popup(tb->panel);
    tb->popup_visible = 0;
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

static void format_time_remaining(const IsdeBattery *bat, char *buf,
                                  size_t bufsz)
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

/* ---------- position popup above the tray icon ---------- */

static void position_popup(TrayBattery *tb)
{
    Panel *p = tb->panel;
    Widget popup = tb->popup_shell;

    if (!popup || !tb->icon) {
        return;
    }

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
    Widget w = tb->icon;
    while (w && w != p->shell) {
        icon_x += w->core.x + w->core.border_width;
        w = w->core.parent;
    }
    int log_mon_x = (int)(p->mon_x / sf + 0.5);
    int log_mon_w = (int)(p->mon_w / sf + 0.5);
    int log_shell_x = p->shell->core.x;

    int icon_center = log_shell_x + icon_x + tb->icon->core.width / 2;
    int x = icon_center - (popup_w + 2 * popup_bw) / 2;
    int y = log_panel_top - popup_h - 2 * popup_bw;

    /* Clamp to monitor bounds */
    if (x + popup_w + 2 * popup_bw > log_mon_x + log_mon_w) {
        x = log_mon_x + log_mon_w - popup_w - 2 * popup_bw;
    }
    if (x < log_mon_x) {
        x = log_mon_x;
    }

    IswConfigureWidget(popup, x, y, popup_w, popup_h, popup_bw);
}

/* ---------- popup init / show / hide / update / cleanup ---------- */

void tbat_popup_init(TrayBattery *tb)
{
    tb->popup_shell = NULL;
    tb->popup_form = NULL;
    tb->popup_visible = 0;
}

void tbat_popup_show(TrayBattery *tb)
{
    Panel *p = tb->panel;
    const IsdeColorScheme *scheme = isde_theme_current();
    const IsdeBattery *bat = tb->power
        ? isde_power_get_battery(tb->power, 0) : NULL;

    if (tb->popup_visible) {
        tbat_popup_hide(tb);
        return;
    }

    if (!tb->popup_shell) {
        IswArgBuilder ab = IswArgBuilderInit();

        /* Override shell */
        IswArgWidth(&ab, 280);
        IswArgHeight(&ab, 220);
        tb->popup_shell = IswCreatePopupShell("batteryPopup",
                                              overrideShellWidgetClass,
                                              p->toplevel, ab.args, ab.count);
        IswAddEventHandler(tb->popup_shell, IswButtonPressMask, False,
                           popup_button_handler, tb);

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
        tb->capacity_label = IswCreateManagedWidget("capLabel",
                                                    labelWidgetClass,
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
        tb->capacity_bar = IswCreateManagedWidget("capBar",
                                                  progressBarWidgetClass,
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
        const char *st = status_text(
            bat ? bat->status : ISDE_BAT_NOT_PRESENT,
            tb->power ? isde_power_on_ac(tb->power) : 0);
        IswArgBuilderReset(&ab);
        IswArgFromVert(&ab, tb->time_label);
        IswArgLabel(&ab, st);
        IswArgBorderWidth(&ab, 0);
        IswArgJustify(&ab, IswJustifyLeft);
        if (scheme) { IswArgForeground(&ab, scheme->fg_dim); }
        if (tb->small_font) { IswArgFont(&ab, tb->small_font); }
        IswArgLeft(&ab, IswChainLeft);
        tb->status_label = IswCreateManagedWidget("statusLabel",
                                                  labelWidgetClass,
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
            Widget section_lbl = IswCreateManagedWidget("sectionHd",
                                                        labelWidgetClass,
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
                                           tb->popup_form,
                                           ab.args, ab.count);
                IswAddCallback(tb->profile_radios[i], IswNcallback,
                               on_profile_toggled, tb);
                prev_radio = tb->profile_radios[i];
                above = tb->profile_radios[i];
            }
        }
    }

    position_popup(tb);
    IswPopup(tb->popup_shell, IswGrabNone);
    IswGrabPointer(tb->popup_shell, True,
                   IswButtonPressMask | IswButtonReleaseMask,
                   IswCursorNone, ISW_CURRENT_TIME);

    panel_show_popup(p, tb->popup_shell);
    tb->popup_visible = 1;
}

void tbat_popup_hide(TrayBattery *tb)
{
    if (!tb->popup_visible) {
        return;
    }

    panel_dismiss_popup(tb->panel);
    tb->popup_visible = 0;
}

void tbat_popup_update(TrayBattery *tb)
{
    if (!tb->popup_visible) {
        return;
    }

    tb->updating = 1;

    const IsdeBattery *bat = tb->power
        ? isde_power_get_battery(tb->power, 0) : NULL;

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
        const char *st = status_text(
            bat ? bat->status : ISDE_BAT_NOT_PRESENT,
            tb->power ? isde_power_on_ac(tb->power) : 0);
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

void tbat_popup_cleanup(TrayBattery *tb)
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

/* ---------- position menu above the tray icon ---------- */

static void position_menu(TrayBattery *tb)
{
    Panel *p = tb->panel;
    Widget menu = tb->menu_shell;

    if (!menu || !tb->icon) {
        return;
    }

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
    Widget w = tb->icon;
    while (w && w != p->shell) {
        icon_x += w->core.x + w->core.border_width;
        w = w->core.parent;
    }
    int log_mon_x = (int)(p->mon_x / sf + 0.5);
    int log_mon_w = (int)(p->mon_w / sf + 0.5);
    int log_shell_x = p->shell->core.x;

    int icon_center = log_shell_x + icon_x + tb->icon->core.width / 2;
    int x = icon_center - (menu_w + 2 * menu_bw) / 2;
    int y = log_panel_top - menu_h - 2 * menu_bw;

    /* Clamp to monitor bounds */
    if (x + menu_w + 2 * menu_bw > log_mon_x + log_mon_w) {
        x = log_mon_x + log_mon_w - menu_w - 2 * menu_bw;
    }
    if (x < log_mon_x) {
        x = log_mon_x;
    }

    IswConfigureWidget(shell, x, y, menu_w, menu_h, menu_bw);
}

void tbat_menu_init(TrayBattery *tb)
{
    tb->menu_shell = NULL;
}

void tbat_menu_show(TrayBattery *tb)
{
    Panel *p = tb->panel;

    /* menu_shell holds the (windowless) SimpleMenu; its popup shell is
     * IswParent(tb->menu_shell). */
    if (tb->menu_shell) {
        IswDestroyWidget(IswParent(tb->menu_shell));
    }

    IswArgBuilder ab = IswArgBuilderInit();
    tb->menu_shell = IswCreateMenuPopupShell("batteryMenu", p->toplevel,
                                             NULL, 0);
    IswAddEventHandler(IswParent(tb->menu_shell), IswButtonPressMask, False,
                       menu_button_handler, p);

    /* Battery info header */
    const IsdeBattery *bat = tb->power
        ? isde_power_get_battery(tb->power, 0) : NULL;
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
            Widget w = IswCreateManagedWidget("profileItem",
                                              smeBSBObjectClass,
                                              tb->menu_shell,
                                              ab.args, ab.count);
            IswAddCallback(w, IswNcallback, on_menu_profile,
                           (IswPointer)(intptr_t)i);
        }
    }

    position_menu(tb);
    Widget shell = IswParent(tb->menu_shell);
    IswPopup(shell, IswGrabNone);
    IswGrabPointer(shell, True,
                   IswButtonPressMask | IswButtonReleaseMask,
                   IswCursorNone, ISW_CURRENT_TIME);

    panel_show_popup(p, shell);
}

void tbat_menu_cleanup(TrayBattery *tb)
{
    if (tb->menu_shell) {
        IswDestroyWidget(IswParent(tb->menu_shell));
        tb->menu_shell = NULL;
    }
}
