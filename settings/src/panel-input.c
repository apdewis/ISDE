#define _POSIX_C_SOURCE 200809L
/*
 * panel-input.c — Input settings: double-click speed
 * Each panel manages its own Save/Revert buttons.
 */
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Widget scale_dclick;
static int    saved_dclick;
static IsdeDBus *panel_dbus;

static void save_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)cd; (void)call;
    int val = IswScaleGetValue(scale_dclick);
    saved_dclick = val;

    char *path = isde_xdg_config_path("isde.toml");
    if (path) {
        isde_config_write_int(path, "input", "double_click_ms", val);
        free(path);
    }
    isde_config_invalidate_cache();

    if (panel_dbus)
        isde_dbus_settings_notify(panel_dbus, "input", "double_click_ms");
}

static void revert_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)cd; (void)call;
    IswScaleSetValue(scale_dclick, saved_dclick);
}

static Widget input_create(Widget parent, XtAppContext app)
{
    (void)app;

    saved_dclick = isde_config_double_click_ms();

    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNdefaultDistance, 8); n++;
    XtSetArg(args[n], XtNborderWidth, 0);    n++;

    /* Get parent dimensions to fill */
    Dimension pw, ph;
    Arg qargs[20];
    XtSetArg(qargs[0], XtNwidth, &pw);
    XtSetArg(qargs[1], XtNheight, &ph);
    XtGetValues(parent, qargs, 2);
    if (pw > 0) { XtSetArg(args[n], XtNwidth, pw); n++; }
    if (ph > 0) { XtSetArg(args[n], XtNheight, ph); n++; }

    Widget form = XtCreateWidget("inputPanel", formWidgetClass,
                                 parent, args, n);

    /* Label */
    n = 0;
    XtSetArg(args[n], XtNlabel, "Double-click speed (ms):"); n++;
    XtSetArg(args[n], XtNborderWidth, 0);                     n++;
    Widget lbl = XtCreateManagedWidget("dclickLbl", labelWidgetClass,
                                       form, args, n);

    /* Scale slider: 100 - 1000 ms */
    n = 0;
    XtSetArg(args[n], XtNfromVert, lbl);              n++;
    XtSetArg(args[n], XtNminimumValue, 100);           n++;
    XtSetArg(args[n], XtNmaximumValue, 1000);          n++;
    XtSetArg(args[n], XtNscaleValue, saved_dclick);    n++;
    XtSetArg(args[n], XtNorientation, XtorientHorizontal); n++;
    XtSetArg(args[n], XtNshowValue, True);             n++;
    XtSetArg(args[n], XtNwidth, 300);                  n++;
    XtSetArg(args[n], XtNborderWidth, 0);              n++;
    scale_dclick = XtCreateManagedWidget("dclickScale", scaleWidgetClass,
                                         form, args, n);

    /* Save / Revert buttons */
    n = 0;
    XtSetArg(args[n], XtNfromVert, scale_dclick); n++;
    XtSetArg(args[n], XtNlabel, "Save");           n++;
    XtSetArg(args[n], XtNborderWidth, 0);          n++;
    Widget save_btn = XtCreateManagedWidget("saveBtn", commandWidgetClass,
                                            form, args, n);
    XtAddCallback(save_btn, XtNcallback, save_cb, NULL);

    n = 0;
    XtSetArg(args[n], XtNfromVert, scale_dclick); n++;
    XtSetArg(args[n], XtNfromHoriz, save_btn);     n++;
    XtSetArg(args[n], XtNlabel, "Revert");         n++;
    XtSetArg(args[n], XtNborderWidth, 0);          n++;
    Widget revert_btn = XtCreateManagedWidget("revertBtn", commandWidgetClass,
                                              form, args, n);
    XtAddCallback(revert_btn, XtNcallback, revert_cb, NULL);

    return form;
}

static int input_has_changes(void)
{
    if (!scale_dclick) return 0;
    return IswScaleGetValue(scale_dclick) != saved_dclick;
}

static void input_destroy(void)
{
    scale_dclick = NULL;
}

/* Called by settings.c after D-Bus init */
void panel_input_set_dbus(IsdeDBus *bus) { panel_dbus = bus; }

const IsdeSettingsPanel panel_input = {
    .name        = "Input",
    .icon        = NULL,
    .section     = "input",
    .create      = input_create,
    .has_changes = input_has_changes,
    .destroy     = input_destroy,
};
