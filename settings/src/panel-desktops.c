#define _POSIX_C_SOURCE 200809L
/*
 * panel-desktops.c — Virtual desktop settings: grid rows and columns
 */
#include "settings.h"
#include <ISW/Slider.h>
#include <ISW/IswArgMacros.h>

#include <stdlib.h>

static Widget scale_rows;
static Widget scale_cols;

static int saved_rows;
static int saved_cols;

static IsdeDBus *panel_dbus;

#define SLIDER_W 300
#define LABEL_W 150

static void desktops_apply(void)
{
    int rows = IswSliderGetValue(scale_rows);
    int cols = IswSliderGetValue(scale_cols);

    char *path = isde_xdg_config_path("isde.toml");
    if (path) {
        isde_config_write_int(path, "wm.desktops", "rows", rows);
        isde_config_write_int(path, "wm.desktops", "columns", cols);
        free(path);
    }

    saved_rows = rows;
    saved_cols = cols;

    if (panel_dbus) {
        isde_dbus_settings_notify(panel_dbus, "wm.desktops", "*");
    }
}

static void desktops_revert(void)
{
    IswSliderSetValue(scale_rows, saved_rows);
    IswSliderSetValue(scale_cols, saved_cols);
}

static void make_scale_row(Widget vbox, const char *label_text,
                           int min, int max, int value, Widget *out_scale)
{
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgOrientation(&ab, IswOrientHorizontal);
    IswArgBorderWidth(&ab, 0);
    IswArgSpacing(&ab, 8);
    Widget row = IswCreateManagedWidget("row", flexBoxWidgetClass,
                                       vbox, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, label_text);
    IswArgBorderWidth(&ab, 0);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgFlexBasis(&ab, LABEL_W);
    IswArgFlexAlign(&ab, IswFlexAlignCenter);
    IswCreateManagedWidget("lbl", labelWidgetClass, row, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgMinimumValue(&ab, min);
    IswArgMaximumValue(&ab, max);
    IswArgSliderValue(&ab, value);
    IswArgOrientation(&ab, IswOrientHorizontal);
    IswArgShowValue(&ab, True);
    IswArgTickInterval(&ab, 1);
    IswArgWidth(&ab, SLIDER_W);
    IswArgBorderWidth(&ab, 0);
    IswArgFlexAlign(&ab, IswFlexAlignCenter);
    *out_scale = IswCreateManagedWidget("slider", sliderWidgetClass,
                                       row, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "");
    IswArgBorderWidth(&ab, 0);
    IswArgFlexBasis(&ab, 20);
    IswCreateManagedWidget("spacer", labelWidgetClass, row, ab.args, ab.count);
}

static Widget desktops_create(Widget parent, IswAppContext app)
{
    (void)app;

    /* Load current values */
    saved_rows = 1;
    saved_cols = 2;
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *wm_tbl = isde_config_table(root, "wm");
        IsdeConfigTable *desk = wm_tbl ? isde_config_table(wm_tbl, "desktops") : NULL;
        if (desk) {
            int r = (int)isde_config_int(desk, "rows", 1);
            int c = (int)isde_config_int(desk, "columns", 2);
            if (r > 0) { saved_rows = r; }
            if (c > 0) { saved_cols = c; }
        }
        isde_config_free(cfg);
    }

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgOrientation(&ab, IswOrientVertical);
    IswArgSpacing(&ab, 8);
    IswArgBorderWidth(&ab, 0);
    Widget vbox = IswCreateWidget("desktopsPanel", flexBoxWidgetClass,
                                 parent, ab.args, ab.count);

    make_scale_row(vbox, "Desktop rows:",
                   1, 4, saved_rows, &scale_rows);
    make_scale_row(vbox, "Desktop columns:",
                   1, 4, saved_cols, &scale_cols);

    return vbox;
}

static int desktops_has_changes(void)
{
    if (!scale_rows) { return 0; }
    return IswSliderGetValue(scale_rows) != saved_rows ||
           IswSliderGetValue(scale_cols) != saved_cols;
}

static void desktops_destroy(void)
{
    scale_rows = NULL;
    scale_cols = NULL;
}

void panel_desktops_set_dbus(IsdeDBus *bus) { panel_dbus = bus; }

const IsdeSettingsPanel panel_desktops = {
    .name        = "Desktops",
    .icon        = "preferences-desktop-wallpaper",
    .section     = "wm.desktops",
    .create      = desktops_create,
    .apply       = desktops_apply,
    .revert      = desktops_revert,
    .has_changes = desktops_has_changes,
    .destroy     = desktops_destroy,
};
