#define _POSIX_C_SOURCE 200809L
/*
 * tray.c — internal tray area for panel module icons
 *
 * Provides a FlexBox container and add/remove API for panel modules
 * to place icon widgets into.  Modules are responsible for implementing
 * their own click handlers and functionality.
 */
#include "panel.h"

#include <ISW/IswArgMacros.h>

void panel_tray_init(Panel *p)
{
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgOrientation(&ab, IswOrientVertical);
    IswArgBorderWidth(&ab, 0);
    IswArgFlexBasis(&ab, 2);
    p->tray_area = IswCreateManagedWidget("trayArea", flexBoxWidgetClass,
                                          p->form, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "");
    IswArgBorderWidth(&ab, 0);
    IswArgFlexGrow(&ab, 1);
    IswCreateManagedWidget("traySpcTop", labelWidgetClass,
                           p->tray_area, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgOrientation(&ab, IswOrientHorizontal);
    IswArgBorderWidth(&ab, 0);
    IswArgFlexGrow(&ab, 2);
    IswArgSpacing(&ab, 2);
    p->tray_box = IswCreateManagedWidget("trayBox", flexBoxWidgetClass,
                                          p->tray_area, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "");
    IswArgBorderWidth(&ab, 0);
    IswArgFlexGrow(&ab, 1);
    IswCreateManagedWidget("traySpcBot", labelWidgetClass,
                           p->tray_area, ab.args, ab.count);
}

Widget panel_tray_add_icon(Panel *p, char *name, WidgetClass wclass)
{
    int icon_size = PANEL_ICON_SIZE;

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgBorderWidth(&ab, 0);
    IswArgResizable(&ab, 0);
    IswArgFlexBasis(&ab, icon_size);
    Widget icon = IswCreateManagedWidget(name, wclass,
                                         p->tray_box, ab.args, ab.count);

    int nchildren = 0;
    Widget *children = NULL;
    IswArgBuilderReset(&ab);
    ISW_ARG(&ab, IswNchildren, &children);
    ISW_ARG(&ab, IswNnumChildren, &nchildren);
    IswGetValues(p->tray_box, ab.args, ab.count);

    int icon_stride = icon_size + 2;
    int tray_w = nchildren * icon_stride + 2;
    IswArgBuilderReset(&ab);
    IswArgFlexBasis(&ab, tray_w);
    IswSetValues(p->tray_area, ab.args, ab.count);

    return icon;
}

void panel_tray_remove_icon(Panel *p, Widget icon)
{
    int icon_size = PANEL_ICON_SIZE;

    IswDestroyWidget(icon);

    int nchildren = 0;
    Widget *children = NULL;
    IswArgBuilder ab = IswArgBuilderInit();
    ISW_ARG(&ab, IswNchildren, &children);
    ISW_ARG(&ab, IswNnumChildren, &nchildren);
    IswGetValues(p->tray_box, ab.args, ab.count);

    int icon_stride = icon_size + 2;
    int tray_w = nchildren > 0 ? nchildren * icon_stride + 2 : 2;
    IswArgBuilderReset(&ab);
    IswArgFlexBasis(&ab, tray_w);
    IswSetValues(p->tray_area, ab.args, ab.count);
}

void panel_tray_cleanup(Panel *p)
{
    (void)p;
}
