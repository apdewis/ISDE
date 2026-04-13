#define _POSIX_C_SOURCE 200809L
/*
 * panel-desktops.c — Virtual desktop settings: grid rows and columns
 */
#include "settings.h"
#include <ISW/Slider.h>

#include <stdlib.h>

static Widget scale_rows;
static Widget scale_cols;

static int saved_rows;
static int saved_cols;

static IsdeDBus *panel_dbus;

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

static int lbl_w;
static int scale_w;

static Widget make_scale_row(Widget form, Widget above, const char *label_text,
                             int min, int max, int value, Widget *out_scale)
{
    Arg args[20];
    Cardinal n;

    n = 0;
    IswSetArg(args[n], IswNlabel, label_text);               n++;
    IswSetArg(args[n], IswNborderWidth, 0);                   n++;
    IswSetArg(args[n], IswNwidth, lbl_w);                     n++;
    IswSetArg(args[n], IswNjustify, IswJustifyRight);         n++;
    IswSetArg(args[n], IswNleft, IswChainLeft);                n++;
    IswSetArg(args[n], IswNright, IswChainLeft);               n++;
    if (above) { IswSetArg(args[n], IswNfromVert, above); n++; }
    Widget lbl = IswCreateManagedWidget("lbl", labelWidgetClass,
                                       form, args, n);

    n = 0;
    IswSetArg(args[n], IswNfromHoriz, lbl);                   n++;
    if (above) { IswSetArg(args[n], IswNfromVert, above); n++; }
    IswSetArg(args[n], IswNminimumValue, min);                 n++;
    IswSetArg(args[n], IswNmaximumValue, max);                 n++;
    IswSetArg(args[n], IswNsliderValue, value);                 n++;
    IswSetArg(args[n], IswNorientation, XtorientHorizontal);   n++;
    IswSetArg(args[n], IswNshowValue, True);                   n++;
    IswSetArg(args[n], IswNtickInterval, 1);                   n++;
    IswSetArg(args[n], IswNwidth, scale_w);                    n++;
    IswSetArg(args[n], IswNborderWidth, 0);                    n++;
    IswSetArg(args[n], IswNresizable, True);                   n++;
    IswSetArg(args[n], IswNleft, IswChainLeft);                 n++;
    IswSetArg(args[n], IswNright, IswChainRight);               n++;
    *out_scale = IswCreateManagedWidget("slider", sliderWidgetClass,
                                       form, args, n);
    return *out_scale;
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

    Dimension pw;
    Arg qa[20];
    IswSetArg(qa[0], IswNwidth, &pw);
    IswGetValues(parent, qa, 1);

    lbl_w = 180;
    scale_w = (pw > 0 ? (int)pw - lbl_w - 8 * 4 : 200);
    if (scale_w < 100) { scale_w = 100; }

    Arg args[20];
    Cardinal n = 0;
    IswSetArg(args[n], IswNdefaultDistance, 8); n++;
    IswSetArg(args[n], IswNborderWidth, 0);    n++;
    Widget form = IswCreateWidget("desktopsPanel", formWidgetClass,
                                 parent, args, n);

    Widget row;
    row = make_scale_row(form, NULL, "Desktop rows:",
                         1, 4, saved_rows, &scale_rows);
    row = make_scale_row(form, row, "Desktop columns:",
                         1, 4, saved_cols, &scale_cols);

    return form;
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
    .icon        = NULL,
    .section     = "wm.desktops",
    .create      = desktops_create,
    .apply       = desktops_apply,
    .revert      = desktops_revert,
    .has_changes = desktops_has_changes,
    .destroy     = desktops_destroy,
};
