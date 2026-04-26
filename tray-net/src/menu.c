#define _POSIX_C_SOURCE 200809L
/*
 * menu.c — popup menu for the tray network applet
 *
 * Builds a SimpleMenu listing ConnMan technologies (with power toggle)
 * and services (with connect/disconnect).
 */
#include "tray-net.h"

#include <ISW/IntrinsicP.h>
#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- callback data ---------- */

enum {
    ACTION_CONNECT = 0,
    ACTION_DISCONNECT,
    ACTION_POWER_ON,
    ACTION_POWER_OFF,
    ACTION_SCAN,
};

typedef struct MenuAction {
    TrayNet    *tn;
    char        path[PATH_LEN];
    int         action;
} MenuAction;

static MenuAction *actions = NULL;
static int nactions = 0;
static int cap_actions = 0;

static MenuAction *alloc_action(TrayNet *tn, const char *path, int action)
{
    if (nactions >= cap_actions) {
        cap_actions = cap_actions ? cap_actions * 2 : 16;
        actions = realloc(actions, cap_actions * sizeof(MenuAction));
    }
    MenuAction *a = &actions[nactions++];
    a->tn = tn;
    snprintf(a->path, sizeof(a->path), "%s", path);
    a->action = action;
    return a;
}

/* ---------- action callback ---------- */

static void on_action(Widget w, IswPointer client_data, IswPointer call_data)
{
    (void)w; (void)call_data;
    MenuAction *a = (MenuAction *)client_data;
    TrayNet *tn = a->tn;

    switch (a->action) {
    case ACTION_CONNECT:
        fprintf(stderr, "isde-tray-net: connecting %s\n", a->path);
        tn_connman_service_connect(tn, a->path);
        break;

    case ACTION_DISCONNECT:
        fprintf(stderr, "isde-tray-net: disconnecting %s\n", a->path);
        tn_connman_service_disconnect(tn, a->path);
        break;

    case ACTION_POWER_ON:
        fprintf(stderr, "isde-tray-net: powering on %s\n", a->path);
        tn_connman_tech_set_powered(tn, a->path, 1);
        break;

    case ACTION_POWER_OFF:
        fprintf(stderr, "isde-tray-net: powering off %s\n", a->path);
        tn_connman_tech_set_powered(tn, a->path, 0);
        break;

    case ACTION_SCAN:
        fprintf(stderr, "isde-tray-net: scanning %s\n", a->path);
        tn_connman_scan(tn, a->path);
        break;
    }
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

/* ---------- helper: service state display string ---------- */

static const char *state_display(const ServiceInfo *s)
{
    if (strcmp(s->state, "online") == 0)     return "online";
    if (strcmp(s->state, "ready") == 0)      return "connected";
    if (strcmp(s->state, "association") == 0) return "connecting...";
    if (strcmp(s->state, "configuration") == 0) return "configuring...";
    if (strcmp(s->state, "disconnect") == 0) return "disconnecting...";
    if (strcmp(s->state, "failure") == 0)    return "failed";
    return NULL;
}

static int is_connected(const ServiceInfo *s)
{
    return strcmp(s->state, "online") == 0 ||
           strcmp(s->state, "ready") == 0;
}

static int is_connecting(const ServiceInfo *s)
{
    return strcmp(s->state, "association") == 0 ||
           strcmp(s->state, "configuration") == 0;
}

/* ---------- menu construction ---------- */

void tn_menu_init(TrayNet *tn)
{
    tn->menu_shell = IswCreatePopupShell("netMenu", simpleMenuWidgetClass,
                                          tn->toplevel, NULL, 0);
}

void tn_menu_show(TrayNet *tn)
{
    if (tn->menu_shell)
        IswDestroyWidget(tn->menu_shell);

    nactions = 0;

    IswArgBuilder ab = IswArgBuilderInit();

    tn->menu_shell = IswCreatePopupShell("netMenu", simpleMenuWidgetClass,
                                          tn->toplevel, NULL, 0);

    if (!tn->connman_available) {
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, "Network manager unavailable");
        IswArgSensitive(&ab, False);
        IswCreateManagedWidget("noConnman", smeBSBObjectClass,
                              tn->menu_shell, ab.args, ab.count);
        goto popup;
    }

    /* Technologies */
    for (int i = 0; i < tn->ntechs; i++) {
        TechInfo *t = &tn->techs[i];

        if (i > 0) {
            IswCreateManagedWidget("sep", smeLineObjectClass,
                                  tn->menu_shell, NULL, 0);
        }

        /* Technology toggle: "WiFi  [Disable]" or "WiFi  [Enable]" */
        char label[384];
        if (t->powered) {
            snprintf(label, sizeof(label), "%s — Disable", t->name);
            IswArgBuilderReset(&ab);
            IswArgLabel(&ab, label);
            Widget w = IswCreateManagedWidget("techToggle", smeBSBObjectClass,
                                              tn->menu_shell,
                                              ab.args, ab.count);
            MenuAction *a = alloc_action(tn, t->path, ACTION_POWER_OFF);
            IswAddCallback(w, IswNcallback, on_action, a);
        } else {
            snprintf(label, sizeof(label), "%s — Enable", t->name);
            IswArgBuilderReset(&ab);
            IswArgLabel(&ab, label);
            Widget w = IswCreateManagedWidget("techToggle", smeBSBObjectClass,
                                              tn->menu_shell,
                                              ab.args, ab.count);
            MenuAction *a = alloc_action(tn, t->path, ACTION_POWER_ON);
            IswAddCallback(w, IswNcallback, on_action, a);
        }

        /* Scan option for wifi */
        if (strcmp(t->type, "wifi") == 0 && t->powered) {
            IswArgBuilderReset(&ab);
            IswArgLabel(&ab, "  Scan");
            Widget w = IswCreateManagedWidget("scan", smeBSBObjectClass,
                                              tn->menu_shell,
                                              ab.args, ab.count);
            MenuAction *a = alloc_action(tn, t->path, ACTION_SCAN);
            IswAddCallback(w, IswNcallback, on_action, a);
        }

        /* Services belonging to this technology */
        int has_services = 0;
        for (int j = 0; j < tn->nservices; j++) {
            ServiceInfo *s = &tn->services[j];
            if (strcmp(s->type, t->type) != 0)
                continue;

            has_services = 1;
            char slabel[512];
            const char *sdisplay = state_display(s);

            if (strcmp(s->type, "wifi") == 0) {
                if (sdisplay) {
                    snprintf(slabel, sizeof(slabel), "  %s (%s, %d%%)",
                             s->name[0] ? s->name : "(hidden)",
                             sdisplay, s->strength);
                } else {
                    snprintf(slabel, sizeof(slabel), "  %s (%d%%)",
                             s->name[0] ? s->name : "(hidden)",
                             s->strength);
                }
            } else {
                if (sdisplay) {
                    snprintf(slabel, sizeof(slabel), "  %s (%s)",
                             s->name[0] ? s->name : s->type, sdisplay);
                } else {
                    snprintf(slabel, sizeof(slabel), "  %s",
                             s->name[0] ? s->name : s->type);
                }
            }

            IswArgBuilderReset(&ab);
            IswArgLabel(&ab, slabel);

            if (is_connected(s) || is_connecting(s)) {
                /* Disconnect option */
                Widget w = IswCreateManagedWidget("svcDisconnect",
                    smeBSBObjectClass, tn->menu_shell, ab.args, ab.count);
                MenuAction *a = alloc_action(tn, s->path, ACTION_DISCONNECT);
                IswAddCallback(w, IswNcallback, on_action, a);
            } else if (s->favorite) {
                /* Known network — can connect without agent */
                Widget w = IswCreateManagedWidget("svcConnect",
                    smeBSBObjectClass, tn->menu_shell, ab.args, ab.count);
                MenuAction *a = alloc_action(tn, s->path, ACTION_CONNECT);
                IswAddCallback(w, IswNcallback, on_action, a);
            } else {
                /* Unknown secured network — no agent yet, show but disable */
                int needs_passphrase = strcmp(s->security, "none") != 0 &&
                                       strcmp(s->security, "") != 0;
                if (needs_passphrase) {
                    IswArgSensitive(&ab, False);
                    IswCreateManagedWidget("svcLocked", smeBSBObjectClass,
                                          tn->menu_shell, ab.args, ab.count);
                } else {
                    Widget w = IswCreateManagedWidget("svcConnect",
                        smeBSBObjectClass, tn->menu_shell,
                        ab.args, ab.count);
                    MenuAction *a = alloc_action(tn, s->path, ACTION_CONNECT);
                    IswAddCallback(w, IswNcallback, on_action, a);
                }
            }
        }

        if (!has_services && t->powered) {
            IswArgBuilderReset(&ab);
            IswArgLabel(&ab, "  No networks found");
            IswArgSensitive(&ab, False);
            IswCreateManagedWidget("noNetworks", smeBSBObjectClass,
                                  tn->menu_shell, ab.args, ab.count);
        }
    }

    if (tn->ntechs == 0) {
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, "No network interfaces");
        IswArgSensitive(&ab, False);
        IswCreateManagedWidget("noTech", smeBSBObjectClass,
                              tn->menu_shell, ab.args, ab.count);
    }

popup:
    if (tn->tray_icon)
        IswTrayIconSetMenu(tn->tray_icon, tn->menu_shell);

    IswPopup(tn->menu_shell, IswGrabExclusive);

    /* Grab pointer for outside-click dismiss */
    {
        xcb_connection_t *conn = IswDisplay(tn->toplevel);
        xcb_grab_pointer(conn, 1, IswWindow(tn->menu_shell),
                         XCB_EVENT_MASK_BUTTON_PRESS |
                         XCB_EVENT_MASK_BUTTON_RELEASE,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                         XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
        xcb_flush(conn);
    }
    IswAddRawEventHandler(tn->menu_shell, 0, True,
                          menu_grab_handler, NULL);
    IswAddCallback(tn->menu_shell, IswNpopdownCallback,
                   menu_popdown_cb, NULL);

    /* Reposition above the tray icon */
    if (tn->tray_icon) {
        xcb_connection_t *conn = IswDisplay(tn->toplevel);
        xcb_window_t icon_win = IswTrayIconGetWindow(tn->tray_icon);
        xcb_window_t root = IswScreen(tn->toplevel)->root;

        xcb_translate_coordinates_cookie_t cookie =
            xcb_translate_coordinates(conn, icon_win, root, 0, 0);
        xcb_translate_coordinates_reply_t *reply =
            xcb_translate_coordinates_reply(conn, cookie, NULL);

        if (reply) {
            double sf = ISWScaleFactor(tn->toplevel);
            int phys_mw = (int)(tn->menu_shell->core.width * sf + 0.5);
            int phys_mh = (int)(tn->menu_shell->core.height * sf + 0.5);
            int phys_bw = (int)(tn->menu_shell->core.border_width * sf + 0.5);
            int total_w = phys_mw + 2 * phys_bw;
            int total_h = phys_mh + 2 * phys_bw;

            int scr_w = IswScreen(tn->toplevel)->width_in_pixels;
            int scr_h = IswScreen(tn->toplevel)->height_in_pixels;

            int x = reply->dst_x;
            int y = reply->dst_y - total_h;

            if (x + total_w > scr_w)
                x = scr_w - total_w;
            if (x < 0) x = 0;
            if (y < 0) y = 0;

            uint32_t vals[] = { (uint32_t)x, (uint32_t)y };
            xcb_configure_window(conn, IswWindow(tn->menu_shell),
                                 XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                                 vals);
            xcb_flush(conn);
            free(reply);
        }
    }
}

void tn_menu_cleanup(TrayNet *tn)
{
    if (tn->menu_shell) {
        IswDestroyWidget(tn->menu_shell);
        tn->menu_shell = NULL;
    }
    free(actions);
    actions = NULL;
    nactions = 0;
    cap_actions = 0;
}
