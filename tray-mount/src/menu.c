#define _POSIX_C_SOURCE 200809L
/*
 * menu.c — popup menu for the tray mount applet
 *
 * Builds a SimpleMenu listing detected removable devices with
 * Mount / Unmount / Eject actions.
 */
#include "tray-mount.h"

#include <ISW/IntrinsicP.h>
#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>

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

/* ---------- dismiss on outside click / ungrab on popdown ---------- */

static void menu_popdown_cb(Widget w, IswPointer client_data,
                            IswPointer call_data)
{
    (void)client_data; (void)call_data;
    xcb_ungrab_pointer(IswDisplay(w), XCB_CURRENT_TIME);
    xcb_flush(IswDisplay(w));
}

static void menu_grab_handler(Widget w, IswPointer closure,
                              xcb_generic_event_t *event,
                              Boolean *cont)
{
    (void)closure;
    uint8_t type = event->response_type & 0x7f;

    if (type == XCB_BUTTON_PRESS) {
        xcb_button_press_event_t *ev = (xcb_button_press_event_t *)event;

        /* event_x/event_y are physical pixels relative to the grab
         * window.  Get physical menu size to compare. */
        double sf = ISWScaleFactor(w);
        int pw = (int)(w->core.width * sf + 0.5);
        int ph = (int)(w->core.height * sf + 0.5);
        if (ev->event_x < 0 || ev->event_y < 0 ||
            ev->event_x >= pw || ev->event_y >= ph) {
            xcb_ungrab_pointer(IswDisplay(w), XCB_CURRENT_TIME);
            xcb_flush(IswDisplay(w));
            IswPopdown(w);
            *cont = False;
            return;
        }
    }
    *cont = True;
}

/* ---------- menu construction ---------- */

void tm_menu_init(TrayMount *tm)
{
    tm->menu_shell = IswCreatePopupShell("mountMenu", simpleMenuWidgetClass,
                                          tm->toplevel, NULL, 0);
}

void tm_menu_show(TrayMount *tm)
{
    /* Destroy and recreate the menu to reflect current device state */
    if (tm->menu_shell) {
        IswDestroyWidget(tm->menu_shell);
    }

    /* Reset action allocations */
    nactions = 0;

    IswArgBuilder ab = IswArgBuilderInit();

    tm->menu_shell = IswCreatePopupShell("mountMenu", simpleMenuWidgetClass,
                                          tm->toplevel, NULL, 0);

    if (tm->ndevices == 0) {
        /* Show "No devices" */
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, "No removable devices");
        IswArgSensitive(&ab, False);
        IswCreateManagedWidget("noDevices", smeBSBObjectClass,
                              tm->menu_shell, ab.args, ab.count);
    } else {
        for (int i = 0; i < tm->ndevices; i++) {
            DeviceInfo *d = &tm->devices[i];

            /* Device header: prefer label, then vendor/model, then dev_path */
            char label[384];
            if (d->label[0]) {
                snprintf(label, sizeof(label), "%s (%s)", d->label,
                         d->dev_path);
            } else if (d->vendor[0]) {
                snprintf(label, sizeof(label), "%s (%s)", d->vendor,
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
            IswArgBuilderReset(&ab);
            IswArgLabel(&ab, label);
            IswArgSensitive(&ab, False);
            IswCreateManagedWidget("devLabel", smeBSBObjectClass,
                                  tm->menu_shell, ab.args, ab.count);

            if (d->is_mounted) {
                /* Unmount action */
                IswArgBuilderReset(&ab);
                IswArgLabel(&ab, "  Unmount");
                Widget w = IswCreateManagedWidget("unmount",
                    smeBSBObjectClass, tm->menu_shell, ab.args, ab.count);
                MenuAction *a = alloc_action(tm, i, ACTION_UNMOUNT);
                IswAddCallback(w, IswNcallback, on_action, a);

                /* Eject action (if ejectable) */
                if (d->is_ejectable) {
                    IswArgBuilderReset(&ab);
                    IswArgLabel(&ab, "  Eject");
                    w = IswCreateManagedWidget("eject",
                        smeBSBObjectClass, tm->menu_shell, ab.args, ab.count);
                    a = alloc_action(tm, i, ACTION_EJECT);
                    IswAddCallback(w, IswNcallback, on_action, a);
                }
            } else {
                /* Mount action */
                IswArgBuilderReset(&ab);
                IswArgLabel(&ab, "  Mount");
                Widget w = IswCreateManagedWidget("mount",
                    smeBSBObjectClass, tm->menu_shell, ab.args, ab.count);
                MenuAction *a = alloc_action(tm, i, ACTION_MOUNT);
                IswAddCallback(w, IswNcallback, on_action, a);
            }
        }
    }

    /* Re-attach to tray icon (we destroyed the old menu_shell above) */
    if (tm->tray_icon) {
        IswTrayIconSetMenu(tm->tray_icon, tm->menu_shell);
    }

    IswRealizeWidget(tm->menu_shell);

    /* Position before mapping to avoid flicker */
    if (tm->tray_icon) {
        xcb_connection_t *conn = IswDisplay(tm->toplevel);
        xcb_window_t icon_win = IswTrayIconGetWindow(tm->tray_icon);
        xcb_window_t root = IswScreen(tm->toplevel)->root;

        xcb_translate_coordinates_cookie_t cookie =
            xcb_translate_coordinates(conn, icon_win, root, 0, 0);
        xcb_translate_coordinates_reply_t *reply =
            xcb_translate_coordinates_reply(conn, cookie, NULL);

        if (reply) {
            double sf = ISWScaleFactor(tm->toplevel);
            int icon_x = (int)(reply->dst_x / sf);
            int icon_y = (int)(reply->dst_y / sf);
            free(reply);

            Dimension w = tm->menu_shell->core.width;
            Dimension h = tm->menu_shell->core.height;
            Dimension bw = tm->menu_shell->core.border_width;
            int total_w = (int)(w + 2 * bw);
            int total_h = (int)(h + 2 * bw);
            int scr_w = (int)(IswScreen(tm->toplevel)->width_in_pixels / sf);

            int x = icon_x;
            int y = icon_y - total_h;

            if (x + total_w > scr_w)
                x = scr_w - total_w;
            if (x < 0) x = 0;
            if (y < 0) y = 0;

            IswConfigureWidget(tm->menu_shell, x, y, w, h, bw);
        }
    }

    IswPopup(tm->menu_shell, IswGrabExclusive);

    /* Grab pointer and register handler to dismiss on outside click */
    {
        xcb_connection_t *conn = IswDisplay(tm->toplevel);
        xcb_grab_pointer(conn, 1, IswWindow(tm->menu_shell),
                         XCB_EVENT_MASK_BUTTON_PRESS |
                         XCB_EVENT_MASK_BUTTON_RELEASE,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                         XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
        xcb_flush(conn);
    }
    IswAddRawEventHandler(tm->menu_shell, 0, True,
                          menu_grab_handler, NULL);
    IswAddCallback(tm->menu_shell, IswNpopdownCallback,
                   menu_popdown_cb, NULL);
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
