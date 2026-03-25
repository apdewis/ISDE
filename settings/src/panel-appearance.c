#define _POSIX_C_SOURCE 200809L
/*
 * panel-appearance.c — Appearance settings: font selection
 */
#include "settings.h"
#include <stdlib.h>
#include <string.h>

static Widget font_chooser;
static char  *saved_family;
static int    saved_size;
static IsdeDBus *panel_dbus;

static void save_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)cd; (void)call;
    const char *family = IswFontChooserGetFamily(font_chooser);
    int size = IswFontChooserGetSize(font_chooser);

    free(saved_family);
    saved_family = strdup(family);
    saved_size = size;

    char *path = isde_xdg_config_path("isde.toml");
    if (path) {
        isde_config_write_string(path, "appearance", "font_family", family);
        isde_config_write_int(path, "appearance", "font_size", size);
        free(path);
    }

    if (panel_dbus)
        isde_dbus_settings_notify(panel_dbus, "appearance", "*");
}

static void revert_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)cd; (void)call;
    /* FontChooser doesn't have a SetFamily/SetSize API —
     * would need to destroy and recreate. For now just note it. */
}

static Widget appearance_create(Widget parent, XtAppContext app)
{
    (void)app;

    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNdefaultDistance, 8); n++;
    XtSetArg(args[n], XtNborderWidth, 0);    n++;
    Dimension pw, ph;
    Arg qargs[20];
    XtSetArg(qargs[0], XtNwidth, &pw);
    XtSetArg(qargs[1], XtNheight, &ph);
    XtGetValues(parent, qargs, 2);
    if (pw > 0) { XtSetArg(args[n], XtNwidth, pw); n++; }
    if (ph > 0) { XtSetArg(args[n], XtNheight, ph); n++; }
    Widget form = XtCreateWidget("appearPanel", formWidgetClass,
                                 parent, args, n);

    n = 0;
    XtSetArg(args[n], XtNlabel, "Default Font:"); n++;
    XtSetArg(args[n], XtNborderWidth, 0);          n++;
    Widget lbl = XtCreateManagedWidget("fontLbl", labelWidgetClass,
                                       form, args, n);

    n = 0;
    XtSetArg(args[n], XtNfromVert, lbl);    n++;
    XtSetArg(args[n], XtNborderWidth, 0);   n++;
    font_chooser = XtCreateManagedWidget("fontChooser", fontChooserWidgetClass,
                                         form, args, n);

    /* Save / Revert buttons */
    n = 0;
    XtSetArg(args[n], XtNfromVert, font_chooser); n++;
    XtSetArg(args[n], XtNlabel, "Save");           n++;
    XtSetArg(args[n], XtNborderWidth, 0);          n++;
    Widget save_btn = XtCreateManagedWidget("saveBtn", commandWidgetClass,
                                            form, args, n);
    XtAddCallback(save_btn, XtNcallback, save_cb, NULL);

    n = 0;
    XtSetArg(args[n], XtNfromVert, font_chooser); n++;
    XtSetArg(args[n], XtNfromHoriz, save_btn);     n++;
    XtSetArg(args[n], XtNlabel, "Revert");         n++;
    XtSetArg(args[n], XtNborderWidth, 0);          n++;
    Widget revert_btn = XtCreateManagedWidget("revertBtn", commandWidgetClass,
                                              form, args, n);
    XtAddCallback(revert_btn, XtNcallback, revert_cb, NULL);

    return form;
}

static int appearance_has_changes(void)
{
    return 0; /* TODO: compare current font selection to saved */
}

static void appearance_destroy(void)
{
    font_chooser = NULL;
    free(saved_family);
    saved_family = NULL;
}

void panel_appearance_set_dbus(IsdeDBus *bus) { panel_dbus = bus; }

const IsdeSettingsPanel panel_appearance = {
    .name        = "Appearance",
    .icon        = NULL,
    .section     = "appearance",
    .create      = appearance_create,
    .has_changes = appearance_has_changes,
    .destroy     = appearance_destroy,
};
