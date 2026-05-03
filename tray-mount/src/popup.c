#define _POSIX_C_SOURCE 200809L
/*
 * popup.c — ListBox popup for the tray mount applet
 *
 * Builds a ListBox listing detected removable devices with
 * Mount / Unmount / Eject action buttons per row.
 */
#include "tray-mount.h"

#include <ISW/IntrinsicP.h>
#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>
#include <isde/isde-tray.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    DeviceInfo *d = &tm->devices[a->device_idx];
    char result[256];

    switch (a->action) {
    case ACTION_MOUNT:
        if (d->is_luks) {
            tm_popup_hide(tm);
            tm_password_dialog_show(tm, a->device_idx);
            return;
        }
        if (tm_dbus_mount(tm, d->dev_path, "", result, sizeof(result)) == 0)
            fprintf(stderr, "isde-tray-mount: mounted %s at %s\n",
                    d->dev_path, result);
        else
            fprintf(stderr, "isde-tray-mount: mount failed: %s\n", result);
        break;

    case ACTION_UNMOUNT:
        if (tm_dbus_unmount(tm, d->dev_path, result, sizeof(result)) == 0)
            fprintf(stderr, "isde-tray-mount: unmounted %s\n", d->dev_path);
        else
            fprintf(stderr, "isde-tray-mount: unmount failed: %s\n", result);
        break;

    case ACTION_EJECT:
        if (tm_dbus_eject(tm, d->dev_path, result, sizeof(result)) == 0)
            fprintf(stderr, "isde-tray-mount: ejected %s\n", d->dev_path);
        else
            fprintf(stderr, "isde-tray-mount: eject failed: %s\n", result);
        break;
    }

    tm_popup_hide(tm);
    tm_dbus_list_devices(tm);
}

/* ---------- popup dismiss ---------- */

#define POPUP_DISMISS_MASK (XCB_EVENT_MASK_BUTTON_PRESS)

static void popup_outside_handler(Widget w, IswPointer closure,
                                  xcb_generic_event_t *event,
                                  Boolean *cont)
{
    (void)cont;
    TrayMount *tm = (TrayMount *)closure;
    uint8_t type = event->response_type & 0x7f;

    if (type != XCB_BUTTON_PRESS)
        return;
    if (!tm->popup_visible || !tm->popup_shell)
        return;

    xcb_button_press_event_t *ev = (xcb_button_press_event_t *)event;
    double sf = ISWScaleFactor(w);
    int pw = (int)(w->core.width * sf + 0.5);
    int ph = (int)(w->core.height * sf + 0.5);

    if (ev->event_x < 0 || ev->event_y < 0 ||
        ev->event_x >= pw || ev->event_y >= ph) {
        tm_popup_hide(tm);
    }
}

/* ---------- position popup above tray icon ---------- */

static void position_popup(TrayMount *tm)
{
    isde_tray_position_popup(tm->toplevel, tm->tray_icon, tm->popup_shell);
}

/* ---------- public API ---------- */

void tm_popup_init(TrayMount *tm)
{
    tm->popup_shell = NULL;
    tm->popup_outer = NULL;
    tm->popup_viewport = NULL;
    tm->popup_visible = 0;
}

void tm_popup_show(TrayMount *tm)
{
    const IsdeColorScheme *scheme = isde_theme_current();
    
    if (tm->popup_visible) {
        tm_popup_hide(tm);
        return;
    }

    if (tm->popup_shell) {
        IswDestroyWidget(tm->popup_shell);
        tm->popup_shell = NULL;
        tm->popup_outer = NULL;
        tm->popup_viewport = NULL;
    }

    nactions = 0;

    IswArgBuilder ab = IswArgBuilderInit();

    /* Override shell */
    IswArgWidth(&ab, 400);
    IswArgHeight(&ab, 400);
    tm->popup_shell = IswCreatePopupShell("mountPopup",
                                          overrideShellWidgetClass,
                                          tm->toplevel, ab.args, ab.count);

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
    IswArgBorderWidth(&ab, 1);
    IswArgBackground(&ab, scheme->bg_light);
    Widget toggle_area = IswCreateManagedWidget("toggleArea", formWidgetClass,
                                                tm->popup_outer,
                                                ab.args, ab.count);

    /* Viewport — fills remaining space */
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
    Widget listbox = IswCreateManagedWidget("deviceList",
                                            listBoxWidgetClass,
                                            tm->popup_viewport,
                                            ab.args, ab.count);

    if (tm->ndevices == 0) {
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, "No removable devices");
        IswArgBorderWidth(&ab, 0);
        IswArgSelectable(&ab, False);
        IswCreateManagedWidget("noDevices", labelWidgetClass,
                              listbox, ab.args, ab.count);
    } else {
        for (int i = 0; i < tm->ndevices; i++) {
            DeviceInfo *d = &tm->devices[i];

            char label[384];
            if (d->label[0])
                snprintf(label, sizeof(label), "%s (%s)%s",
                         d->label, d->dev_path,
                         d->is_luks ? " [encrypted]" : "");
            else if (d->vendor[0])
                snprintf(label, sizeof(label), "%s (%s)%s",
                         d->vendor, d->dev_path,
                         d->is_luks ? " [encrypted]" : "");
            else
                snprintf(label, sizeof(label), "%s%s", d->dev_path,
                         d->is_luks ? " [encrypted]" : "");

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
                Widget w = IswCreateManagedWidget("unmount",
                    commandWidgetClass, row, ab.args, ab.count);
                free(icon);
                MenuAction *a = alloc_action(tm, i, ACTION_UNMOUNT);
                IswAddCallback(w, IswNcallback, on_action, a);

                if (d->is_ejectable) {
                    icon = isde_icon_find("actions", "media-eject");
                    IswArgBuilderReset(&ab);
                    IswArgLabel(&ab, "");
                    IswArgJustify(&ab, IswJustifyRight);
                    if (icon)
                        IswArgImage(&ab, icon);
                    w = IswCreateManagedWidget("eject",
                        commandWidgetClass, row, ab.args, ab.count);
                    free(icon);
                    a = alloc_action(tm, i, ACTION_EJECT);
                    IswAddCallback(w, IswNcallback, on_action, a);
                }
            } else {
                char *icon = isde_icon_find("actions", "media-mount");
                IswArgBuilderReset(&ab);
                IswArgLabel(&ab, "");
                IswArgJustify(&ab, IswJustifyRight);
                if (icon)
                    IswArgImage(&ab, icon);
                Widget w = IswCreateManagedWidget("mount",
                    commandWidgetClass, row, ab.args, ab.count);
                free(icon);
                MenuAction *a = alloc_action(tm, i, ACTION_MOUNT);
                IswAddCallback(w, IswNcallback, on_action, a);
            }
        }
    }

    IswRealizeWidget(tm->popup_shell);
    position_popup(tm);
    IswPopup(tm->popup_shell, IswGrabNone);

    {
        xcb_connection_t *conn = IswDisplay(tm->toplevel);
        xcb_grab_pointer(conn, True, IswWindow(tm->popup_shell),
                         XCB_EVENT_MASK_BUTTON_PRESS |
                         XCB_EVENT_MASK_BUTTON_RELEASE,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                         XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
        xcb_flush(conn);
    }

    IswAddEventHandler(tm->popup_shell, POPUP_DISMISS_MASK, False,
                       popup_outside_handler, tm);
    tm->popup_visible = 1;
}

void tm_popup_hide(TrayMount *tm)
{
    if (!tm->popup_visible)
        return;

    if (tm->popup_shell)
        IswRemoveEventHandler(tm->popup_shell, POPUP_DISMISS_MASK, False,
                              popup_outside_handler, tm);

    xcb_ungrab_pointer(IswDisplay(tm->toplevel), XCB_CURRENT_TIME);
    xcb_flush(IswDisplay(tm->toplevel));

    if (tm->popup_shell)
        IswPopdown(tm->popup_shell);

    tm->popup_visible = 0;
}

void tm_popup_cleanup(TrayMount *tm)
{
    if (tm->popup_shell) {
        IswDestroyWidget(tm->popup_shell);
        tm->popup_shell = NULL;
        tm->popup_outer = NULL;
        tm->popup_viewport = NULL;
    }
    tm->popup_visible = 0;

    free(actions);
    actions = NULL;
    nactions = 0;
    cap_actions = 0;
}
