/*
 * tray-battery.h — isde-tray-battery internal header
 */
#ifndef ISDE_TRAY_BATTERY_H
#define ISDE_TRAY_BATTERY_H

#include <ISW/Intrinsic.h>
#include <ISW/StringDefs.h>
#include <ISW/Shell.h>
#include <ISW/SimpleMenu.h>
#include <ISW/SmeBSB.h>
#include <ISW/SmeLine.h>
#include <ISW/Command.h>
#include <ISW/Toggle.h>
#include <ISW/Label.h>
#include <ISW/FlexBox.h>
#include <ISW/Form.h>
#include <ISW/Box.h>
#include <ISW/Slider.h>
#include <ISW/IswTrayIcon.h>
#include <ISW/ISWSVG.h>
#include <ISW/ProgressBar.h>

#include <xcb/xcb.h>

#include "isde-dbus.h"
#include "isde-theme.h"
#include "isde-config.h"
#include "isde-xdg.h"
#include "isde-power.h"
#include "isde-cpufreq.h"

/* ---------- applet state ---------- */

typedef struct TrayBattery {
    IswAppContext        app;
    Widget               toplevel;

    /* Tray icon */
    IswTrayIcon          tray_icon;
    int                  icon_state;     /* encoded capacity+charging for change detection */

    /* Popup */
    Widget               popup_shell;
    Widget               popup_form;
    Widget               capacity_label;
    Widget               capacity_bar;
    Widget               time_label;
    Widget               status_label;
    Widget               profile_radios[3];
    int                  popup_visible;
    int                  updating;

    /* Right-click menu */
    Widget               menu_shell;

    /* Power state */
    IsdePower           *power;
    IswIntervalId        poll_timer;

    /* Small font */
    IswFontStruct       *small_font;

    /* Session D-Bus (theme changes) */
    IsdeDBus            *session_dbus;

    int                  running;
    int                  restart;
} TrayBattery;

/* ---------- tray-battery.c ---------- */
int  tray_battery_init(TrayBattery *tb, int *argc, char **argv);
void tray_battery_run(TrayBattery *tb);
void tray_battery_cleanup(TrayBattery *tb);
void tray_battery_update_icon(TrayBattery *tb);

/* ---------- popup.c ---------- */
void tb_popup_init(TrayBattery *tb);
void tb_popup_show(TrayBattery *tb);
void tb_popup_hide(TrayBattery *tb);
void tb_popup_update(TrayBattery *tb);
void tb_popup_cleanup(TrayBattery *tb);

void tb_menu_init(TrayBattery *tb);
void tb_menu_show(TrayBattery *tb);
void tb_menu_cleanup(TrayBattery *tb);

#endif /* ISDE_TRAY_BATTERY_H */
