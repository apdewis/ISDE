#define _POSIX_C_SOURCE 200809L
/*
 * panel-display.c — Display settings (placeholder for xrandr)
 */
#include "settings.h"
#include <stdlib.h>

static Widget display_create(Widget parent, XtAppContext app)
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
    Widget form = XtCreateWidget("displayPanel", formWidgetClass,
                                 parent, args, n);

    n = 0;
    XtSetArg(args[n], XtNlabel, "Display configuration (xrandr) - coming soon"); n++;
    XtSetArg(args[n], XtNborderWidth, 0); n++;
    XtCreateManagedWidget("placeholder", labelWidgetClass, form, args, n);

    return form;
}

static int display_has_changes(void) { return 0; }
static void display_destroy(void) {}

const IsdeSettingsPanel panel_display = {
    .name        = "Display",
    .icon        = NULL,
    .section     = "display",
    .create      = display_create,
    .has_changes = display_has_changes,
    .destroy     = display_destroy,
};
