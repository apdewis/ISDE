#define _POSIX_C_SOURCE 200809L
/*
 * panel-power.c — Power settings: DPMS, idle suspend, lid action, CPU profile
 */
#include "settings.h"

#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <ISW/IswArgMacros.h>
#include <ISW/ComboBox.h>

#include "dpms.h"
#include "isde-cpufreq.h"

static Widget scale_screen_off;
static Widget scale_idle_suspend;
static Widget lid_combo;
static Widget profile_combo;

static int saved_screen_off;
static int saved_idle_suspend;
static int saved_lid_action;     /* index into lid_action_labels */
static int saved_profile;        /* index into profile_labels */

static int sel_lid_action;
static int sel_profile;

static IsdeDBus *panel_dbus;

#define SLIDER_W 300
#define LABEL_W  250
#define COMBO_W  200

static String lid_action_labels[] = {
    "Suspend", "Hibernate", "Lock", "Nothing"
};
static const char *lid_action_values[] = {
    "suspend", "hibernate", "lock", "nothing"
};
#define NUM_LID_ACTIONS 4

static String profile_labels[] = {
    "Power Saver", "Balanced", "Performance"
};
static const char *profile_values[] = {
    "power_saver", "balanced", "performance"
};
#define NUM_PROFILES 3

static int lid_action_from_string(const char *s)
{
    for (int i = 0; i < NUM_LID_ACTIONS; i++) {
        if (strcmp(s, lid_action_values[i]) == 0) {
            return i;
        }
    }
    return 0;
}

static int profile_from_string(const char *s)
{
    for (int i = 0; i < NUM_PROFILES; i++) {
        if (strcmp(s, profile_values[i]) == 0) {
            return i;
        }
    }
    return 1;
}

/* ---------- combo callbacks ---------- */

static void on_lid_select(Widget w, IswPointer client_data,
                          IswPointer call_data)
{
    (void)w; (void)client_data;
    IswListReturnStruct *ret = (IswListReturnStruct *)call_data;
    sel_lid_action = ret->list_index;
}

static void on_profile_select(Widget w, IswPointer client_data,
                               IswPointer call_data)
{
    (void)w; (void)client_data;
    IswListReturnStruct *ret = (IswListReturnStruct *)call_data;
    sel_profile = ret->list_index;
}

/* ---------- panel ops ---------- */

static void power_apply(void)
{
    int screen_off = IswSliderGetValue(scale_screen_off);
    int idle_suspend = IswSliderGetValue(scale_idle_suspend);

    char *path = isde_xdg_config_path("isde.toml");
    if (path) {
        isde_config_write_int(path, "power", "screen_off_sec", screen_off);
        isde_config_write_int(path, "power", "idle_suspend_sec", idle_suspend);
        isde_config_write_string(path, "power", "lid_action",
                                  lid_action_values[sel_lid_action]);
        isde_config_write_string(path, "power", "profile",
                                  profile_values[sel_profile]);
        free(path);
    }

    saved_screen_off = screen_off;
    saved_idle_suspend = idle_suspend;
    saved_lid_action = sel_lid_action;
    saved_profile = sel_profile;

    isde_cpufreq_set_profile((IsdeCpuProfile)sel_profile);

    isde_config_invalidate_cache();
    if (panel_dbus) {
        isde_dbus_settings_notify(panel_dbus, "power", "*");
    }
}

static void power_revert(void)
{
    IswSliderSetValue(scale_screen_off, saved_screen_off);
    IswSliderSetValue(scale_idle_suspend, saved_idle_suspend);

    sel_lid_action = saved_lid_action;
    if (lid_combo) {
        IswListHighlight(lid_combo, saved_lid_action);
    }

    sel_profile = saved_profile;
    if (profile_combo) {
        IswListHighlight(profile_combo, saved_profile);
    }
}

static Widget make_scale_row(Widget form, Widget above, const char *label_text,
                             int min, int max, int value, Widget *out_scale)
{
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgLabel(&ab, label_text);
    IswArgBorderWidth(&ab, 0);
    IswArgWidth(&ab, LABEL_W);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    if (above) { IswArgFromVert(&ab, above); }
    Widget lbl = IswCreateManagedWidget("lbl", labelWidgetClass,
                                       form, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgFromHoriz(&ab, lbl);
    if (above) { IswArgFromVert(&ab, above); }
    IswArgMinimumValue(&ab, min);
    IswArgMaximumValue(&ab, max);
    IswArgSliderValue(&ab, value);
    IswArgOrientation(&ab, IswOrientHorizontal);
    IswArgShowValue(&ab, True);
    IswArgWidth(&ab, SLIDER_W);
    IswArgBorderWidth(&ab, 0);
    IswArgLeft(&ab, IswChainLeft);
    *out_scale = IswCreateManagedWidget("slider", sliderWidgetClass,
                                       form, ab.args, ab.count);
    return *out_scale;
}

static Widget make_combo_row(Widget form, Widget above, const char *label_text,
                             String *items, int nitems, int selected,
                             Widget *out_combo, IswCallbackProc cb)
{
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgLabel(&ab, label_text);
    IswArgBorderWidth(&ab, 0);
    IswArgWidth(&ab, LABEL_W);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    if (above) { IswArgFromVert(&ab, above); }
    Widget lbl = IswCreateManagedWidget("lbl", labelWidgetClass,
                                       form, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgFromHoriz(&ab, lbl);
    if (above) { IswArgFromVert(&ab, above); }
    IswArgList(&ab, items);
    IswArgNumberStrings(&ab, nitems);
    IswArgDefaultColumns(&ab, 1);
    IswArgForceColumns(&ab, True);
    IswArgVerticalList(&ab, True);
    IswArgWidth(&ab, COMBO_W);
    IswArgLeft(&ab, IswChainLeft);
    *out_combo = IswCreateManagedWidget("combo", comboBoxWidgetClass,
                                        form, ab.args, ab.count);
    IswAddCallback(*out_combo, IswNcallback, cb, NULL);
    IswListHighlight(*out_combo, selected);

    return *out_combo;
}

static Widget power_create(Widget parent, IswAppContext app)
{
    (void)app;

    saved_screen_off = 600;
    saved_idle_suspend = 0;
    saved_lid_action = 0;
    saved_profile = 1;

    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *power = isde_config_table(root, "power");
        if (power) {
            saved_screen_off = (int)isde_config_int(power, "screen_off_sec",
                                                     600);
            saved_idle_suspend = (int)isde_config_int(power, "idle_suspend_sec",
                                                       0);
            const char *la = isde_config_string(power, "lid_action", "suspend");
            saved_lid_action = lid_action_from_string(la);
            const char *pr = isde_config_string(power, "profile", "balanced");
            saved_profile = profile_from_string(pr);
        }
        isde_config_free(cfg);
    }

    sel_lid_action = saved_lid_action;
    sel_profile = saved_profile;

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgDefaultDistance(&ab, 8);
    IswArgBorderWidth(&ab, 0);
    Widget form = IswCreateWidget("powerPanel", formWidgetClass,
                                 parent, ab.args, ab.count);

    Widget row;
    row = make_scale_row(form, NULL, "Screen off (seconds):",
                         0, 3600, saved_screen_off, &scale_screen_off);
    row = make_scale_row(form, row, "Idle suspend (seconds):",
                         0, 3600, saved_idle_suspend, &scale_idle_suspend);
    row = make_combo_row(form, row, "Lid close action:",
                         lid_action_labels, NUM_LID_ACTIONS,
                         saved_lid_action, &lid_combo, on_lid_select);

    if (isde_cpufreq_available()) {
        row = make_combo_row(form, row, "Performance profile:",
                             profile_labels, NUM_PROFILES,
                             saved_profile, &profile_combo, on_profile_select);
    }

    return form;
}

static int power_has_changes(void)
{
    if (!scale_screen_off) { return 0; }
    return IswSliderGetValue(scale_screen_off) != saved_screen_off ||
           IswSliderGetValue(scale_idle_suspend) != saved_idle_suspend ||
           sel_lid_action != saved_lid_action ||
           sel_profile != saved_profile;
}

static void power_destroy(void)
{
    scale_screen_off = NULL;
    scale_idle_suspend = NULL;
    lid_combo = NULL;
    profile_combo = NULL;
}

void panel_power_set_dbus(IsdeDBus *bus) { panel_dbus = bus; }

const IsdeSettingsPanel panel_power = {
    .name        = "Power",
    .icon        = "preferences-system-power-management",
    .section     = "power",
    .create      = power_create,
    .apply       = power_apply,
    .revert      = power_revert,
    .has_changes = power_has_changes,
    .destroy     = power_destroy,
};
