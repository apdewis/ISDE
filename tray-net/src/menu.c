#define _POSIX_C_SOURCE 200809L
/*
 * menu.c — popup for the tray network applet
 *
 * Layout:
 *   overrideShell
 *     Form (outer)
 *       Box: WiFi toggle, Bluetooth toggle — single row
 *       Viewport
 *         ListBox
 *           "Connected" heading (non-selectable separator row)
 *           ListBoxRow per connected service: label + Disconnect button
 *           "Available" heading (non-selectable separator row)
 *           ListBoxRow per available service: label + Connect button
 */
#include "tray-net.h"

#include <ISW/IntrinsicP.h>
#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>

#include "isde/isde-xdg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- callback data ---------- */

typedef struct MenuAction {
    TrayNet    *tn;
    char        path[PATH_LEN];
} MenuAction;

typedef struct ToggleData {
    TrayNet    *tn;
    char        path[PATH_LEN];
} ToggleData;

static MenuAction *actions = NULL;
static int nactions = 0;
static int cap_actions = 0;

static ToggleData *toggle_data = NULL;
static int ntoggles = 0;
static int cap_toggles = 0;

static MenuAction *alloc_action(TrayNet *tn, const char *path)
{
    if (nactions >= cap_actions) {
        cap_actions = cap_actions ? cap_actions * 2 : 16;
        actions = realloc(actions, cap_actions * sizeof(MenuAction));
    }
    MenuAction *a = &actions[nactions++];
    a->tn = tn;
    snprintf(a->path, sizeof(a->path), "%s", path);
    return a;
}

static ToggleData *alloc_toggle(TrayNet *tn, const char *path)
{
    if (ntoggles >= cap_toggles) {
        cap_toggles = cap_toggles ? cap_toggles * 2 : 4;
        toggle_data = realloc(toggle_data, cap_toggles * sizeof(ToggleData));
    }
    ToggleData *t = &toggle_data[ntoggles++];
    t->tn = tn;
    snprintf(t->path, sizeof(t->path), "%s", path);
    return t;
}

/* ---------- callbacks ---------- */

static void on_tech_toggled(Widget w, IswPointer client_data,
                            IswPointer call_data)
{
    (void)call_data;
    ToggleData *td = (ToggleData *)client_data;

    Boolean state = False;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgState(&ab, &state);
    IswGetValues(w, ab.args, ab.count);

    tn_connman_tech_set_powered(td->tn, td->path, state ? 1 : 0);
}

static void on_connect(Widget w, IswPointer client_data, IswPointer call_data)
{
    (void)w; (void)call_data;
    MenuAction *a = (MenuAction *)client_data;
    tn_connman_service_connect(a->tn, a->path);
}

static void on_disconnect(Widget w, IswPointer client_data,
                          IswPointer call_data)
{
    (void)w; (void)call_data;
    MenuAction *a = (MenuAction *)client_data;
    tn_connman_service_disconnect(a->tn, a->path);
}

/* ---------- popup dismiss ---------- */

#define POPUP_DISMISS_MASK (XCB_EVENT_MASK_BUTTON_PRESS)

static void popup_outside_handler(Widget w, IswPointer closure,
                                  xcb_generic_event_t *event,
                                  Boolean *cont)
{
    (void)cont;
    TrayNet *tn = (TrayNet *)closure;
    uint8_t type = event->response_type & 0x7f;

    if (type != XCB_BUTTON_PRESS)
        return;
    if (!tn->popup_visible || !tn->popup_shell)
        return;

    xcb_button_press_event_t *ev = (xcb_button_press_event_t *)event;
    double sf = ISWScaleFactor(w);
    int pw = (int)(w->core.width * sf + 0.5);
    int ph = (int)(w->core.height * sf + 0.5);

    if (ev->event_x < 0 || ev->event_y < 0 ||
        ev->event_x >= pw || ev->event_y >= ph) {
        tn_menu_hide(tn);
    }
}

/* ---------- helpers ---------- */

static int is_connected(const ServiceInfo *s)
{
    return strcmp(s->state, "online") == 0 ||
           strcmp(s->state, "ready") == 0 ||
           strcmp(s->state, "association") == 0 ||
           strcmp(s->state, "configuration") == 0;
}

static const char *signal_icon_name(const ServiceInfo *s)
{
    int secure = strcmp(s->security, "none") != 0 && s->security[0] != '\0';

    if (strcmp(s->type, "wifi") != 0)
        return "network-wired";

    if (s->strength >= 75)
        return secure ? "network-wireless-signal-excellent-secure"
                      : "network-wireless-signal-excellent";
    if (s->strength >= 50)
        return secure ? "network-wireless-signal-good-secure"
                      : "network-wireless-signal-good";
    if (s->strength >= 25)
        return secure ? "network-wireless-signal-ok-secure"
                      : "network-wireless-signal-ok";
    if (s->strength > 0)
        return secure ? "network-wireless-signal-weak-secure"
                      : "network-wireless-signal-weak";

    return secure ? "network-wireless-signal-none-secure"
                  : "network-wireless-signal-none";
}

static const char *state_suffix(const ServiceInfo *s)
{
    if (strcmp(s->state, "online") == 0)         return "online";
    if (strcmp(s->state, "ready") == 0)          return "connected";
    if (strcmp(s->state, "association") == 0)     return "connecting...";
    if (strcmp(s->state, "configuration") == 0)   return "configuring...";
    if (strcmp(s->state, "disconnect") == 0)      return "disconnecting...";
    if (strcmp(s->state, "failure") == 0)         return "failed";
    return NULL;
}

/* ---------- position popup above tray icon ---------- */

static void position_popup(TrayNet *tn)
{
    if (!tn->tray_icon)
        return;

    xcb_connection_t *conn = IswDisplay(tn->toplevel);
    xcb_window_t icon_win = IswTrayIconGetWindow(tn->tray_icon);
    xcb_window_t root = IswScreen(tn->toplevel)->root;

    xcb_translate_coordinates_cookie_t cookie =
        xcb_translate_coordinates(conn, icon_win, root, 0, 0);
    xcb_translate_coordinates_reply_t *reply =
        xcb_translate_coordinates_reply(conn, cookie, NULL);

    if (!reply)
        return;

    double sf = ISWScaleFactor(tn->toplevel);
    int icon_x = (int)(reply->dst_x / sf);
    int icon_y = (int)(reply->dst_y / sf);
    free(reply);

    Dimension w = tn->popup_shell->core.width;
    Dimension h = tn->popup_shell->core.height;
    Dimension bw = tn->popup_shell->core.border_width;
    int total_w = (int)(w + 2 * bw);
    int total_h = (int)(h + 2 * bw);
    int scr_w = (int)(IswScreen(tn->toplevel)->width_in_pixels / sf);

    int x = icon_x;
    int y = icon_y - total_h;

    if (x + total_w > scr_w)
        x = scr_w - total_w;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    IswConfigureWidget(tn->popup_shell, x, y, w, h, bw);
}

/* ---------- clear listbox children ---------- */

static void clear_listbox(Widget listbox)
{
    WidgetList children;
    Cardinal num;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgBuilderAdd(&ab, IswNchildren, (IswArgVal)&children);
    IswArgBuilderAdd(&ab, IswNnumChildren, (IswArgVal)&num);
    IswGetValues(listbox, ab.args, ab.count);

    for (int i = (int)num - 1; i >= 0; i--)
        IswDestroyWidget(children[i]);
}

/* ---------- add a section heading row ---------- */

static void add_heading(Widget listbox, const char *text)
{
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgSelectable(&ab, false);
    IswArgBorderWidth(&ab, 0);
    Widget row = IswCreateWidget("hdrRow", listBoxRowWidgetClass,
                                  listbox, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, text);
    IswArgBorderWidth(&ab, 0);
    IswArgJustify(&ab, IswJustifyLeft);
    IswCreateManagedWidget("hdrLabel", labelWidgetClass,
                            row, ab.args, ab.count);

    IswManageChild(row);
}

/* ---------- add a network service row ---------- */

static void add_service_row(TrayNet *tn, Widget listbox,
                            const ServiceInfo *s, int connected)
{
    IswArgBuilder ab = IswArgBuilderInit();

    char display[512];
    const char *suffix = state_suffix(s);

    if (strcmp(s->type, "wifi") == 0) {
        snprintf(display, sizeof(display), "%s",
                 s->name[0] ? s->name : "(hidden)");
    } else if (strcmp(s->type, "ethernet") == 0 && s->interface[0]) {
        snprintf(display, sizeof(display), "Wired (%s)", s->interface);
    } else {
        snprintf(display, sizeof(display), "%s",
                     s->name[0] ? s->name : s->type);
    }

    IswArgBuilderAdd(&ab, IswNselectable, (IswArgVal)False);
    IswArgBuilderAdd(&ab, IswNlistBoxRowHeight, (IswArgVal)40);
    IswArgBorderWidth(&ab, 0);
    Widget row = IswCreateWidget("netRow", listBoxRowWidgetClass,
                                  listbox, ab.args, ab.count);

    /* Signal icon on the left */
    char *icon_path = isde_icon_find("status", signal_icon_name(s));
    if (icon_path) {
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, "");
        IswArgImage(&ab, icon_path);
        IswArgBorderWidth(&ab, 0);
        IswCreateManagedWidget("netIcon", labelWidgetClass,
                                row, ab.args, ab.count);
        free(icon_path);
    }

    /* Network name label */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, display);
    IswArgBorderWidth(&ab, 0);
    IswArgJustify(&ab, IswJustifyLeft);
    IswCreateManagedWidget("netName", labelWidgetClass,
                            row, ab.args, ab.count);

    /* Button on the right */
    const char *btn_label = connected ? "Disconnect" : "Connect";

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, btn_label);
    IswArgBorderWidth(&ab, 1);
    IswArgJustify(&ab, IswJustifyRight);
    Widget btn = IswCreateManagedWidget("netBtn", commandWidgetClass,
                                         row, ab.args, ab.count);
    MenuAction *a = alloc_action(tn, s->path);
    IswAddCallback(btn, IswNcallback,
                   connected ? on_disconnect : on_connect, a);

    IswManageChild(row);
}

/* ---------- build listbox content ---------- */

static void build_content(TrayNet *tn)
{
    Widget listbox = tn->popup_listbox;

    if (!tn->connman_available) {
        add_heading(listbox, "Network manager unavailable");
        return;
    }

    /* Connected services */
    int has_connected = 0;
    for (int i = 0; i < tn->nservices; i++) {
        if (is_connected(&tn->services[i])) {
            if (!has_connected)
                add_heading(listbox, "Connected");
            has_connected = 1;
            add_service_row(tn, listbox, &tn->services[i], 1);
        }
    }
    add_heading(listbox, "");
    /* Available services */
    int has_available = 0;
    for (int i = 0; i < tn->nservices; i++) {
        if (!is_connected(&tn->services[i])) {
            if (!has_available)
                add_heading(listbox, "Available");
            has_available = 1;
            add_service_row(tn, listbox, &tn->services[i], 0);
        }
    }

    if (!has_connected && !has_available)
        add_heading(listbox, "No networks");
}

/* ---------- public API ---------- */

void tn_menu_init(TrayNet *tn)
{
    tn->popup_shell = NULL;
    tn->popup_outer = NULL;
    tn->popup_viewport = NULL;
    tn->popup_listbox = NULL;
    tn->popup_visible = 0;
}

void tn_menu_show(TrayNet *tn)
{
    /* Pane background tones from theme */
    const IsdeColorScheme *scheme = isde_theme_current();

    if (tn->popup_visible) {
        tn_menu_hide(tn);
        return;
    }

    if (tn->popup_shell) {
        IswDestroyWidget(tn->popup_shell);
        tn->popup_shell = NULL;
        tn->popup_outer = NULL;
        tn->popup_viewport = NULL;
        tn->popup_listbox = NULL;
    }

    nactions = 0;
    ntoggles = 0;

    IswArgBuilder ab = IswArgBuilderInit();

    /* Override shell */
    IswArgWidth(&ab, 400);
    IswArgHeight(&ab, 400);
    tn->popup_shell = IswCreatePopupShell("netPopup",
                                           overrideShellWidgetClass,
                                           tn->toplevel, ab.args, ab.count);

    /* Outer vertical FlexBox */
    IswArgBuilderReset(&ab);
    IswArgOrientation(&ab, IswOrientVertical);
    IswArgBorderWidth(&ab, 0);
    tn->popup_outer = IswCreateManagedWidget("outerBox", flexBoxWidgetClass,
                                              tn->popup_shell,
                                              ab.args, ab.count);    

    /* Toggle row: horizontal Box with WiFi power toggles */
    IswArgBuilderReset(&ab);
    IswArgOrientation(&ab, IswOrientHorizontal);
    IswArgFlexBasis(&ab, 50);
    IswArgBorderWidth(&ab, 1);
    IswArgBackground(&ab, scheme->bg_light);
    Widget toggle_area = IswCreateManagedWidget("toggleArea", formWidgetClass,
                                                tn->popup_outer,
                                                ab.args, ab.count);
    
    for (int i = 0; i < tn->ntechs; i++) {
        TechInfo *t = &tn->techs[i];
        if (strcmp(t->type, "wifi") != 0)
            continue;

        ToggleData *td = alloc_toggle(tn, t->path);

        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, t->name);
        IswArgState(&ab, t->powered ? True : False);
        IswArgJustify(&ab, IswJustifyLeft);
        IswArgBackground(&ab, scheme->bg_light);
        Widget tw = IswCreateManagedWidget("techToggle", toggleWidgetClass,
                                            toggle_area, ab.args, ab.count);
        IswAddCallback(tw, IswNcallback, on_tech_toggled, td);
    }

    /* Viewport — fills remaining space */
    IswArgBuilderReset(&ab);
    IswArgFlexGrow(&ab, 1);
    IswArgForceBars(&ab, True);
    IswArgBorderWidth(&ab, 0);
    IswArgBuilderAdd(&ab, IswNallowVert, (IswArgVal)True);
    IswArgBuilderAdd(&ab, IswNallowHoriz, (IswArgVal)False);
    IswArgBuilderAdd(&ab, IswNuseRight, (IswArgVal)True);
    tn->popup_viewport = IswCreateManagedWidget("viewport",
                                                 viewportWidgetClass,
                                                 tn->popup_outer,
                                                 ab.args, ab.count);

    /* ListBox inside viewport */
    IswArgBuilderReset(&ab);
    IswArgBuilderAdd(&ab, IswNselectionMode,
                     (IswArgVal)IswListBoxSelectNone);
    IswArgBuilderAdd(&ab, IswNshowSeparators, (IswArgVal)True);
    IswArgBuilderAdd(&ab, IswNrowSpacing, (IswArgVal)2);
    tn->popup_listbox = IswCreateManagedWidget("netList",
                                                listBoxWidgetClass,
                                                tn->popup_viewport,
                                                ab.args, ab.count);

    build_content(tn);

    IswRealizeWidget(tn->popup_shell);
    position_popup(tn);
    IswPopup(tn->popup_shell, IswGrabNone);

    {
        xcb_connection_t *conn = IswDisplay(tn->toplevel);
        xcb_grab_pointer(conn, True, IswWindow(tn->popup_shell),
                         XCB_EVENT_MASK_BUTTON_PRESS |
                         XCB_EVENT_MASK_BUTTON_RELEASE,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                         XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
        xcb_flush(conn);
    }

    IswAddEventHandler(tn->popup_shell, POPUP_DISMISS_MASK, False,
                       popup_outside_handler, tn);
    tn->popup_visible = 1;
}

void tn_menu_hide(TrayNet *tn)
{
    if (!tn->popup_visible)
        return;

    if (tn->popup_shell)
        IswRemoveEventHandler(tn->popup_shell, POPUP_DISMISS_MASK, False,
                              popup_outside_handler, tn);

    xcb_ungrab_pointer(IswDisplay(tn->toplevel), XCB_CURRENT_TIME);
    xcb_flush(IswDisplay(tn->toplevel));

    if (tn->popup_shell)
        IswPopdown(tn->popup_shell);

    tn->popup_visible = 0;
}

void tn_menu_rebuild(TrayNet *tn)
{
    if (!tn->popup_visible || !tn->popup_listbox)
        return;

    nactions = 0;
    ntoggles = 0;

    /* Update toggle states */
    WidgetList outer_children;
    Cardinal outer_num;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgBuilderAdd(&ab, IswNchildren, (IswArgVal)&outer_children);
    IswArgBuilderAdd(&ab, IswNnumChildren, (IswArgVal)&outer_num);
    IswGetValues(tn->popup_outer, ab.args, ab.count);

    if (outer_num > 0) {
        Widget toggle_box = outer_children[0];
        WidgetList box_children;
        Cardinal box_num;
        IswArgBuilderReset(&ab);
        IswArgBuilderAdd(&ab, IswNchildren, (IswArgVal)&box_children);
        IswArgBuilderAdd(&ab, IswNnumChildren, (IswArgVal)&box_num);
        IswGetValues(toggle_box, ab.args, ab.count);

        int ti = 0;
        for (int i = 0; i < tn->ntechs && ti < (int)box_num; i++) {
            TechInfo *t = &tn->techs[i];
            if (strcmp(t->type, "wifi") != 0 &&
                strcmp(t->type, "bluetooth") != 0)
                continue;

            ToggleData *td = alloc_toggle(tn, t->path);

            IswArgBuilderReset(&ab);
            IswArgState(&ab, t->powered ? True : False);
            IswSetValues(box_children[ti], ab.args, ab.count);

            IswRemoveAllCallbacks(box_children[ti], IswNcallback);
            IswAddCallback(box_children[ti], IswNcallback,
                           on_tech_toggled, td);
            ti++;
        }
    }

    clear_listbox(tn->popup_listbox);
    build_content(tn);

    //position_popup(tn);
}

void tn_menu_cleanup(TrayNet *tn)
{
    if (tn->popup_shell) {
        IswDestroyWidget(tn->popup_shell);
        tn->popup_shell = NULL;
        tn->popup_outer = NULL;
        tn->popup_viewport = NULL;
        tn->popup_listbox = NULL;
    }
    tn->popup_visible = 0;

    free(actions);
    actions = NULL;
    nactions = 0;
    cap_actions = 0;

    free(toggle_data);
    toggle_data = NULL;
    ntoggles = 0;
    cap_toggles = 0;
}
