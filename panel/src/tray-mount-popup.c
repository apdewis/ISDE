#define _POSIX_C_SOURCE 200809L
/*
 * tray-mount-popup.c — ListBox popup for the mount tray module
 *
 * Builds a ListBox listing detected removable devices with
 * Mount / Unmount / Eject action buttons per row.
 */
#include "tray-mount.h"

#include <ISW/IntrinsicP.h>
#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void popup_button_handler(Widget w, IswPointer client_data,
                                 IswEvent *event, Boolean *cont)
{
    (void)w; (void)cont;
    TrayMount *tm = (TrayMount *)client_data;
    if (event->kind != IswButtonDown)
        return;
    panel_dismiss_popup(tm->panel);
    tm->popup_visible = 0;
}

/* ---------- callback data ---------- */

typedef struct MenuAction {
    TrayMount  *tm;
    int         device_idx;
    int         action;  /* 0 = mount, 1 = unmount, 2 = eject */
} MenuAction;

#define ACTION_MOUNT   0
#define ACTION_UNMOUNT 1
#define ACTION_EJECT   2

static MenuAction *actions = NULL;
static int nactions = 0;
static int cap_actions = 0;

static MenuAction *alloc_action(TrayMount *tm, int dev_idx, int action)
{
    if (nactions >= cap_actions) {
        cap_actions = cap_actions ? cap_actions * 2 : 16;
        actions = realloc(actions, cap_actions * sizeof(MenuAction));
    }
    MenuAction *a = &actions[nactions++];
    a->tm = tm;
    a->device_idx = dev_idx;
    a->action = action;
    return a;
}

/* ---------- action callback ---------- */

static void on_action(Widget w, IswPointer client_data, IswPointer call_data)
{
    (void)w; (void)call_data;
    MenuAction *a = (MenuAction *)client_data;
    TrayMount *tm = a->tm;

    if (a->device_idx < 0 || a->device_idx >= tm->ndevices)
        return;

    MountDeviceInfo *d = &tm->devices[a->device_idx];

    switch (a->action) {
    case ACTION_MOUNT:
        if (d->is_luks) {
            tm_popup_hide(tm);
            tm_password_dialog_show(tm, a->device_idx);
            return;
        }
        tm_dbus_mount(tm, d->dev_path, "");
        break;

    case ACTION_UNMOUNT:
        tm_dbus_unmount(tm, d->dev_path);
        break;

    case ACTION_EJECT:
        tm_dbus_eject(tm, d->dev_path);
        break;
    }

    tm_popup_hide(tm);
}

/* ---------- position popup above the tray icon ---------- */

static void position_popup(TrayMount *tm)
{
    Panel *p = tm->panel;
    Widget popup = tm->popup_shell;

    if (!popup || !tm->icon)
        return;

    double sf = ISWScaleFactor(p->toplevel);
    int log_panel_top = (int)((p->mon_y + p->mon_h) / sf + 0.5) - PANEL_HEIGHT;

    if (!IswIsRealized(popup)) {
        IswRealizeWidget(popup);
    }

    int popup_w = popup->core.width;
    int popup_h = popup->core.height;
    int popup_bw = popup->core.border_width;

    /* Get the icon's position relative to the panel shell */
    int icon_x = 0;
    Widget w = tm->icon;
    while (w && w != p->shell) {
        icon_x += w->core.x + w->core.border_width;
        w = w->core.parent;
    }
    int log_mon_x = (int)(p->mon_x / sf + 0.5);
    int log_mon_w = (int)(p->mon_w / sf + 0.5);
    int log_shell_x = p->shell->core.x;

    int x = log_shell_x + icon_x;
    int y = log_panel_top - popup_h - 2 * popup_bw;

    /* Clamp to monitor bounds */
    if (x + popup_w + 2 * popup_bw > log_mon_x + log_mon_w)
        x = log_mon_x + log_mon_w - popup_w - 2 * popup_bw;
    if (x < log_mon_x)
        x = log_mon_x;

    IswConfigureWidget(popup, x, y, popup_w, popup_h, popup_bw);
}

/* ---------- device list population ---------- */

static void populate_device_list(TrayMount *tm)
{
    Widget listbox = tm->popup_listbox;
    CompositeWidget cw = (CompositeWidget)listbox;

    nactions = 0;

    WidgetList children;
    Cardinal num;
    IswArgBuilder qab = IswArgBuilderInit();
    IswArgBuilderAdd(&qab, IswNchildren, (IswArgVal)&children);
    IswArgBuilderAdd(&qab, IswNnumChildren, (IswArgVal)&num);
    IswGetValues(listbox, qab.args, qab.count);

    for (int i = (int)num - 1; i >= 0; i--) {
        IswDestroyWidget(children[i]);
    }

    IswArgBuilder ab = IswArgBuilderInit();

    if (tm->ndevices == 0) {
        IswArgLabel(&ab, "No removable devices");
        IswArgBorderWidth(&ab, 0);
        IswArgSelectable(&ab, False);
        IswCreateManagedWidget("noDevices", labelWidgetClass,
                              listbox, ab.args, ab.count);
    } else {
        for (int i = 0; i < tm->ndevices; i++) {
            MountDeviceInfo *d = &tm->devices[i];

            char label[384];
            if (d->label[0])
                snprintf(label, sizeof(label), "%s (%s)",
                         d->label, d->dev_path);
            else if (d->vendor[0])
                snprintf(label, sizeof(label), "%s (%s)",
                         d->vendor, d->dev_path);
            else
                snprintf(label, sizeof(label), "%s", d->dev_path);

            IswArgBuilderReset(&ab);
            IswArgBorderWidth(&ab, 0);
            IswArgInternalWidth(&ab, 8);
            IswArgRowPadding(&ab, 8);
            Widget row = IswCreateManagedWidget("devRow",
                listBoxRowWidgetClass, listbox, ab.args, ab.count);

            IswArgBuilderReset(&ab);
            IswArgLabel(&ab, label);
            IswArgBorderWidth(&ab, 0);
            IswArgJustify(&ab, IswJustifyLeft);
            IswCreateManagedWidget("devLabel", labelWidgetClass,
                                  row, ab.args, ab.count);

            if (d->is_mounted) {
                char *icon = isde_icon_find("actions", "media-unmount");
                IswArgBuilderReset(&ab);
                IswArgLabel(&ab, "");
                IswArgJustify(&ab, IswJustifyRight);
                if (icon)
                    IswArgImage(&ab, icon);
                Widget btn = IswCreateManagedWidget("unmount",
                    commandWidgetClass, row, ab.args, ab.count);
                free(icon);
                MenuAction *a = alloc_action(tm, i, ACTION_UNMOUNT);
                IswAddCallback(btn, IswNcallback, on_action, a);

                if (d->is_ejectable) {
                    icon = isde_icon_find("actions", "media-eject");
                    IswArgBuilderReset(&ab);
                    IswArgLabel(&ab, "");
                    IswArgJustify(&ab, IswJustifyRight);
                    if (icon)
                        IswArgImage(&ab, icon);
                    btn = IswCreateManagedWidget("eject",
                        commandWidgetClass, row, ab.args, ab.count);
                    free(icon);
                    a = alloc_action(tm, i, ACTION_EJECT);
                    IswAddCallback(btn, IswNcallback, on_action, a);
                }
            } else {
                char *icon = isde_icon_find("actions", "media-mount");
                IswArgBuilderReset(&ab);
                IswArgLabel(&ab, "");
                IswArgJustify(&ab, IswJustifyRight);
                if (icon)
                    IswArgImage(&ab, icon);
                Widget btn = IswCreateManagedWidget("mount",
                    commandWidgetClass, row, ab.args, ab.count);
                free(icon);
                MenuAction *a = alloc_action(tm, i, ACTION_MOUNT);
                IswAddCallback(btn, IswNcallback, on_action, a);
            }
        }
    }
}

/* ---------- public API ---------- */

void tm_popup_init(TrayMount *tm)
{
    tm->popup_shell = NULL;
    tm->popup_outer = NULL;
    tm->popup_viewport = NULL;
    tm->popup_listbox = NULL;
    tm->popup_visible = 0;
}

void tm_popup_show(TrayMount *tm)
{
    Panel *p = tm->panel;
    const IsdeColorScheme *scheme = isde_theme_current();

    if (tm->popup_visible) {
        tm_popup_hide(tm);
        return;
    }

    nactions = 0;

    if (!tm->popup_shell) {
        IswArgBuilder ab = IswArgBuilderInit();

        /* Override shell */
        IswArgWidth(&ab, 400);
        IswArgHeight(&ab, 400);
        tm->popup_shell = IswCreatePopupShell("mountPopup",
                                            overrideShellWidgetClass,
                                            p->toplevel, ab.args, ab.count);
        IswAddEventHandler(tm->popup_shell, IswButtonPressMask, False,
                           popup_button_handler, tm);

        /* Outer vertical FlexBox */
        IswArgBuilderReset(&ab);
        IswArgOrientation(&ab, IswOrientVertical);
        IswArgBorderWidth(&ab, 0);
        tm->popup_outer = IswCreateManagedWidget("outerBox", flexBoxWidgetClass,
                                                tm->popup_shell,
                                                ab.args, ab.count);

        /* Toggle row: horizontal Box */
        IswArgBuilderReset(&ab);
        IswArgOrientation(&ab, IswOrientHorizontal);
        IswArgFlexBasis(&ab, 50);
        IswArgBorderBottom(&ab, 1);
        if (scheme)
            IswArgBackground(&ab, scheme->bg_light);
        IswCreateManagedWidget("toggleArea", formWidgetClass,
                               tm->popup_outer,
                               ab.args, ab.count);

        /* Viewport -- fills remaining space */
        IswArgBuilderReset(&ab);
        IswArgFlexGrow(&ab, 1);
        IswArgForceBars(&ab, True);
        IswArgBorderWidth(&ab, 0);
        IswArgBuilderAdd(&ab, IswNallowVert, (IswArgVal)True);
        IswArgBuilderAdd(&ab, IswNallowHoriz, (IswArgVal)False);
        IswArgBuilderAdd(&ab, IswNuseRight, (IswArgVal)True);
        tm->popup_viewport = IswCreateManagedWidget("viewport",
                                                    viewportWidgetClass,
                                                    tm->popup_outer,
                                                    ab.args, ab.count);

        /* ListBox inside viewport */
        IswArgBuilderReset(&ab);
        IswArgSelectionMode(&ab, IswListBoxSelectNone);
        IswArgRowSpacing(&ab, 0);
        IswArgBorderWidth(&ab, 0);
        tm->popup_listbox = IswCreateManagedWidget("deviceList",
                                                listBoxWidgetClass,
                                                tm->popup_viewport,
                                                ab.args, ab.count);

        populate_device_list(tm);
    } else {
        populate_device_list(tm);
    }

    position_popup(tm);
    IswPopup(tm->popup_shell, IswGrabNone);
    IswGrabPointer(tm->popup_shell, True,
                   IswButtonPressMask | IswButtonReleaseMask,
                   IswCursorNone, ISW_CURRENT_TIME);

    panel_show_popup(p, tm->popup_shell);
    tm->popup_visible = 1;
}

void tm_popup_refresh(TrayMount *tm)
{
    if (!tm->popup_visible)
        return;
    populate_device_list(tm);
}

void tm_popup_hide(TrayMount *tm)
{
    if (!tm->popup_visible)
        return;

    panel_dismiss_popup(tm->panel);
    tm->popup_visible = 0;
}

void tm_popup_cleanup(TrayMount *tm)
{
    if (tm->popup_shell) {
        IswDestroyWidget(tm->popup_shell);
        tm->popup_shell = NULL;
        tm->popup_outer = NULL;
        tm->popup_viewport = NULL;
        tm->popup_listbox = NULL;
    }
    tm->popup_visible = 0;

    free(actions);
    actions = NULL;
    nactions = 0;
    cap_actions = 0;
}
