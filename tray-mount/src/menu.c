#define _POSIX_C_SOURCE 200809L
/*
 * menu.c — popup menu for the tray mount applet
 *
 * Builds a SimpleMenu listing detected removable devices with
 * Mount / Unmount / Eject actions.
 */
#include "tray-mount.h"

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

    if (a->device_idx < 0 || a->device_idx >= tm->ndevices) {
        return;
    }

    DeviceInfo *d = &tm->devices[a->device_idx];
    char result[256];

    switch (a->action) {
    case ACTION_MOUNT:
        if (tm_dbus_mount(tm, d->dev_path, result, sizeof(result)) == 0) {
            fprintf(stderr, "isde-tray-mount: mounted %s at %s\n",
                    d->dev_path, result);
        } else {
            fprintf(stderr, "isde-tray-mount: mount failed: %s\n", result);
        }
        break;

    case ACTION_UNMOUNT:
        if (tm_dbus_unmount(tm, d->dev_path, result, sizeof(result)) == 0) {
            fprintf(stderr, "isde-tray-mount: unmounted %s\n", d->dev_path);
        } else {
            fprintf(stderr, "isde-tray-mount: unmount failed: %s\n", result);
        }
        break;

    case ACTION_EJECT:
        if (tm_dbus_eject(tm, d->dev_path, result, sizeof(result)) == 0) {
            fprintf(stderr, "isde-tray-mount: ejected %s\n", d->dev_path);
        } else {
            fprintf(stderr, "isde-tray-mount: eject failed: %s\n", result);
        }
        break;
    }

    /* Refresh device list */
    tm_dbus_list_devices(tm);
}

/* ---------- menu construction ---------- */

void tm_menu_init(TrayMount *tm)
{
    Arg args[20];
    Cardinal n = 0;

    tm->menu_shell = IswCreatePopupShell("mountMenu", simpleMenuWidgetClass,
                                          tm->toplevel, args, n);
}

void tm_menu_show(TrayMount *tm)
{
    /* Destroy and recreate the menu to reflect current device state */
    if (tm->menu_shell) {
        IswDestroyWidget(tm->menu_shell);
    }

    /* Reset action allocations */
    nactions = 0;

    Arg args[20];
    Cardinal n = 0;

    tm->menu_shell = IswCreatePopupShell("mountMenu", simpleMenuWidgetClass,
                                          tm->toplevel, args, n);

    if (tm->ndevices == 0) {
        /* Show "No devices" */
        n = 0;
        IswSetArg(args[n], IswNlabel, "No removable devices"); n++;
        IswSetArg(args[n], IswNsensitive, False); n++;
        IswCreateManagedWidget("noDevices", smeBSBObjectClass,
                              tm->menu_shell, args, n);
    } else {
        for (int i = 0; i < tm->ndevices; i++) {
            DeviceInfo *d = &tm->devices[i];

            /* Device header label: "LABEL (dev)" */
            char label[384];
            if (d->label[0]) {
                snprintf(label, sizeof(label), "%s (%s)", d->label,
                         d->dev_path);
            } else {
                snprintf(label, sizeof(label), "%s", d->dev_path);
            }

            /* Separator between devices */
            if (i > 0) {
                IswCreateManagedWidget("sep", smeLineObjectClass,
                                      tm->menu_shell, NULL, 0);
            }

            /* Device label (non-interactive) */
            n = 0;
            IswSetArg(args[n], IswNlabel, label); n++;
            IswSetArg(args[n], IswNsensitive, False); n++;
            IswCreateManagedWidget("devLabel", smeBSBObjectClass,
                                  tm->menu_shell, args, n);

            if (d->is_mounted) {
                /* Unmount action */
                n = 0;
                IswSetArg(args[n], IswNlabel, "  Unmount"); n++;
                Widget w = IswCreateManagedWidget("unmount",
                    smeBSBObjectClass, tm->menu_shell, args, n);
                MenuAction *a = alloc_action(tm, i, ACTION_UNMOUNT);
                IswAddCallback(w, IswNcallback, on_action, a);

                /* Eject action (if ejectable) */
                if (d->is_ejectable) {
                    n = 0;
                    IswSetArg(args[n], IswNlabel, "  Eject"); n++;
                    w = IswCreateManagedWidget("eject",
                        smeBSBObjectClass, tm->menu_shell, args, n);
                    a = alloc_action(tm, i, ACTION_EJECT);
                    IswAddCallback(w, IswNcallback, on_action, a);
                }
            } else {
                /* Mount action */
                n = 0;
                IswSetArg(args[n], IswNlabel, "  Mount"); n++;
                Widget w = IswCreateManagedWidget("mount",
                    smeBSBObjectClass, tm->menu_shell, args, n);
                MenuAction *a = alloc_action(tm, i, ACTION_MOUNT);
                IswAddCallback(w, IswNcallback, on_action, a);
            }
        }
    }

    /* Re-attach to tray icon (we destroyed the old menu_shell above) */
    if (tm->tray_icon) {
        IswTrayIconSetMenu(tm->tray_icon, tm->menu_shell);
    }

    IswPopup(tm->menu_shell, IswGrabExclusive);
}

void tm_menu_cleanup(TrayMount *tm)
{
    if (tm->menu_shell) {
        IswDestroyWidget(tm->menu_shell);
        tm->menu_shell = NULL;
    }
    free(actions);
    actions = NULL;
    nactions = 0;
    cap_actions = 0;
}
