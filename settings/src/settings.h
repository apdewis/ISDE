/*
 * settings.h — isde-settings internal header
 */
#ifndef ISDE_SETTINGS_H
#define ISDE_SETTINGS_H

#include <ISW/Intrinsic.h>
#include <ISW/IntrinsicP.h>
#include <ISW/StringDefs.h>
#include <ISW/MainWindow.h>
#include <ISW/MenuBar.h>
#include <ISW/MenuButton.h>
#include <ISW/SimpleMenu.h>
#include <ISW/SmeBSB.h>
#include <ISW/Command.h>
#include <ISW/Label.h>
#include <ISW/Form.h>
#include <ISW/Box.h>
#include <ISW/Slider.h>
#include <ISW/SpinBox.h>
#include <ISW/FontChooser.h>
#include <ISW/Tabs.h>
#include <ISW/Viewport.h>

#include "isde/isde-config.h"
#include "isde/isde-dbus.h"
#include "isde/isde-theme.h"
#include "isde/isde-xdg.h"
#include "isde/isde-settings-plugin.h"

#define MAX_PANELS 32

typedef struct Settings {
    IswAppContext   app;
    Widget         toplevel;
    Widget         main_window;
    Widget         panel_bar;     /* Top row: panel selector buttons */
    Widget         content_form;  /* Right pane form */
    Widget         content_vp;    /* Scrollable viewport for panel content */
    Widget         content_area;  /* Form inside viewport — panels go here */
    Widget         save_btn;
    Widget         revert_btn;

    /* Panels (core + plugins) */
    const IsdeSettingsPanel *panels[MAX_PANELS];
    Widget                   panel_btns[MAX_PANELS];
    Widget                   panel_widgets[MAX_PANELS]; /* created widget trees */
    void                    *plugin_handles[MAX_PANELS]; /* dlopen handles */
    int                      npanels;
    int                      active_panel;

    /* D-Bus */
    IsdeDBus      *dbus;

    int            running;
} Settings;

/* settings.c */
int   settings_init(Settings *s, int *argc, char **argv);
void  settings_run(Settings *s);
void  settings_cleanup(Settings *s);
void  settings_switch_panel(Settings *s, int index);

/* Core panel registration */
extern const IsdeSettingsPanel panel_input;
extern const IsdeSettingsPanel panel_keyboard;
extern const IsdeSettingsPanel panel_appearance;
extern const IsdeSettingsPanel panel_display;
extern const IsdeSettingsPanel panel_desktops;
extern const IsdeSettingsPanel panel_fonts;
extern const IsdeSettingsPanel panel_dm;

void panel_appearance_set_dbus(IsdeDBus *bus);
void panel_display_set_dbus(IsdeDBus *bus);
void panel_desktops_set_dbus(IsdeDBus *bus);
void panel_fonts_set_dbus(IsdeDBus *bus);

#endif /* ISDE_SETTINGS_H */
