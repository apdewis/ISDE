/*
 * tray-battery.h — battery tray module for isde-panel
 */
#ifndef ISDE_TRAY_BATTERY_H
#define ISDE_TRAY_BATTERY_H

#include "panel.h"

#include <ISW/Toggle.h>
#include <ISW/ProgressBar.h>
#include <ISW/Slider.h>

#include "isde-power.h"
#include "isde-cpufreq.h"

/* ---------- Tray module state ---------- */

typedef struct TrayBattery {
    struct Panel       *panel;

    Widget              icon;
    int                 icon_state;     /* encoded capacity+charging for change detection */

    /* Popup */
    Widget              popup_shell;
    Widget              popup_form;
    Widget              capacity_label;
    Widget              capacity_bar;
    Widget              time_label;
    Widget              status_label;
    Widget              profile_radios[3];
    int                 popup_visible;
    int                 updating;

    /* Right-click menu (windowless SimpleMenu; shell is IswParent(menu_shell)) */
    Widget              menu_shell;

    /* Power state */
    IsdePower          *power;
    IswIntervalId       poll_timer;

    /* Small font */
    IswFontStruct      *small_font;
} TrayBattery;

/* ---------- tray-battery.c ---------- */
void tn_battery_init(struct Panel *p);
void tn_battery_cleanup(struct Panel *p);
void tn_battery_update_icon(TrayBattery *tb);
void tn_battery_reload_theme(TrayBattery *tb);

/* ---------- tray-battery-popup.c ---------- */
void tbat_popup_init(TrayBattery *tb);
void tbat_popup_show(TrayBattery *tb);
void tbat_popup_hide(TrayBattery *tb);
void tbat_popup_update(TrayBattery *tb);
void tbat_popup_cleanup(TrayBattery *tb);

void tbat_menu_init(TrayBattery *tb);
void tbat_menu_show(TrayBattery *tb);
void tbat_menu_cleanup(TrayBattery *tb);

#endif /* ISDE_TRAY_BATTERY_H */
