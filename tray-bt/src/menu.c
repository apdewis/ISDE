#define _POSIX_C_SOURCE 200809L
/*
 * menu.c — popup for the tray bluetooth applet
 *
 * Layout:
 *   overrideShell
 *     FlexBox (vertical)
 *       Form: Bluetooth power toggle + Scan button
 *       Viewport
 *         ListBox
 *           "Connected" heading
 *           ListBoxRow per connected device: icon + name + Disconnect
 *           "Paired" heading
 *           ListBoxRow per paired device: icon + name + Connect
 *           "Available" heading
 *           ListBoxRow per available device: icon + name + Pair
 */
#include "tray-bt.h"

#include <ISW/IntrinsicP.h>
#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>
#include <isde/isde-tray.h>

#include "isde/isde-xdg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- callback data ---------- */

typedef struct MenuAction {
    TrayBt     *tb;
    char        path[PATH_LEN];
} MenuAction;

static MenuAction *actions = NULL;
static int nactions = 0;
static int cap_actions = 0;
static int in_action = 0;

static MenuAction *alloc_action(TrayBt *tb, const char *path)
{
    if (nactions >= cap_actions) {
        cap_actions = cap_actions ? cap_actions * 2 : 16;
        actions = realloc(actions, cap_actions * sizeof(MenuAction));
    }
    MenuAction *a = &actions[nactions++];
    a->tb = tb;
    snprintf(a->path, sizeof(a->path), "%s", path);
    return a;
}

/* ---------- callbacks ---------- */

static void on_power_toggled(Widget w, IswPointer client_data,
                             IswPointer call_data)
{
    (void)call_data;
    TrayBt *tb = (TrayBt *)client_data;

    Boolean state = False;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgState(&ab, &state);
    IswGetValues(w, ab.args, ab.count);

    tb_bluez_set_powered(tb, state ? 1 : 0);
}

static void on_scan(Widget w, IswPointer client_data, IswPointer call_data)
{
    (void)w; (void)call_data;
    TrayBt *tb = (TrayBt *)client_data;

    if (tb->adapter.discovering)
        tb_bluez_stop_discovery(tb);
    else
        tb_bluez_start_discovery(tb);
}

static void on_connect(Widget w, IswPointer client_data, IswPointer call_data)
{
    (void)call_data;
    MenuAction *a = (MenuAction *)client_data;
    DeviceInfo *dev = tb_bluez_find_device(a->tb, a->path);
    if (dev)
        dev->busy = 1;
    in_action = 1;
    if (tb_bluez_device_trust(a->tb, a->path) == 0 && dev)
        dev->trusted = 1;
    tb_bluez_device_connect(a->tb, a->path);
    in_action = 0;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgLabel(&ab, "Connecting...");
    IswArgBuilderAdd(&ab, IswNsensitive, (IswArgVal)False);
    IswSetValues(w, ab.args, ab.count);
}

static void on_disconnect(Widget w, IswPointer client_data,
                          IswPointer call_data)
{
    (void)call_data;
    MenuAction *a = (MenuAction *)client_data;
    DeviceInfo *dev = tb_bluez_find_device(a->tb, a->path);
    if (dev)
        dev->busy = 1;
    tb_bluez_device_disconnect(a->tb, a->path);
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgLabel(&ab, "Disconnecting...");
    IswArgBuilderAdd(&ab, IswNsensitive, (IswArgVal)False);
    IswSetValues(w, ab.args, ab.count);
}

static void on_pair(Widget w, IswPointer client_data, IswPointer call_data)
{
    (void)call_data;
    MenuAction *a = (MenuAction *)client_data;
    DeviceInfo *dev = tb_bluez_find_device(a->tb, a->path);
    if (dev)
        dev->busy = 1;
    in_action = 1;
    if (tb_bluez_device_trust(a->tb, a->path) == 0 && dev)
        dev->trusted = 1;
    tb_bluez_device_pair(a->tb, a->path);
    in_action = 0;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgLabel(&ab, "Pairing...");
    IswArgBuilderAdd(&ab, IswNsensitive, (IswArgVal)False);
    IswSetValues(w, ab.args, ab.count);
}

/* ---------- popup dismiss ---------- */

#define POPUP_DISMISS_MASK (XCB_EVENT_MASK_BUTTON_PRESS)

static void popup_outside_handler(Widget w, IswPointer closure,
                                  xcb_generic_event_t *event,
                                  Boolean *cont)
{
    (void)cont;
    TrayBt *tb = (TrayBt *)closure;
    uint8_t type = event->response_type & 0x7f;

    if (type != XCB_BUTTON_PRESS)
        return;
    if (!tb->popup_visible || !tb->popup_shell)
        return;

    xcb_button_press_event_t *ev = (xcb_button_press_event_t *)event;
    double sf = ISWScaleFactor(w);
    int pw = (int)(w->core.width * sf + 0.5);
    int ph = (int)(w->core.height * sf + 0.5);

    if (ev->event_x < 0 || ev->event_y < 0 ||
        ev->event_x >= pw || ev->event_y >= ph) {
        tb_menu_hide(tb);
    }
}

/* ---------- helpers ---------- */

static const char *device_icon_name(const DeviceInfo *d)
{
    if (d->icon[0] == '\0')
        return "bluetooth-active";

    if (strcmp(d->icon, "audio-card") == 0 ||
        strcmp(d->icon, "audio-headphones") == 0 ||
        strcmp(d->icon, "audio-headset") == 0)
        return "audio-headphones";
    if (strcmp(d->icon, "input-keyboard") == 0)
        return "input-keyboard";
    if (strcmp(d->icon, "input-mouse") == 0)
        return "input-mouse";
    if (strcmp(d->icon, "input-gaming") == 0)
        return "input-gaming";
    if (strcmp(d->icon, "phone") == 0)
        return "phone";

    return "bluetooth-active";
}

static const char *device_display_name(const DeviceInfo *d)
{
    if (d->name[0] != '\0')
        return d->name;
    if (d->address[0] != '\0')
        return d->address;
    return "(unknown)";
}

/* ---------- position popup above tray icon ---------- */

static void position_popup(TrayBt *tb)
{
    isde_tray_position_popup(tb->toplevel, tb->tray_icon, tb->popup_shell);
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

/* ---------- add a device row ---------- */

static void add_device_row(TrayBt *tb, Widget listbox,
                           const DeviceInfo *d, const char *btn_label,
                           IswCallbackProc btn_cb)
{
    IswArgBuilder ab = IswArgBuilderInit();

    IswArgBuilderAdd(&ab, IswNselectable, (IswArgVal)False);
    IswArgBuilderAdd(&ab, IswNlistBoxRowHeight, (IswArgVal)40);
    IswArgBorderWidth(&ab, 0);
    Widget row = IswCreateWidget("btRow", listBoxRowWidgetClass,
                                  listbox, ab.args, ab.count);

    /* Device type icon */
    char *icon_path = isde_icon_find("status", device_icon_name(d));
    if (!icon_path)
        icon_path = isde_icon_find("devices", device_icon_name(d));
    if (icon_path) {
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, "");
        IswArgImage(&ab, icon_path);
        IswArgBorderWidth(&ab, 0);
        IswCreateManagedWidget("btIcon", labelWidgetClass,
                                row, ab.args, ab.count);
        free(icon_path);
    }

    /* Device name */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, device_display_name(d));
    IswArgBorderWidth(&ab, 0);
    IswArgJustify(&ab, IswJustifyLeft);
    IswCreateManagedWidget("btName", labelWidgetClass,
                            row, ab.args, ab.count);

    /* Action button */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, btn_label);
    IswArgBorderWidth(&ab, 1);
    IswArgJustify(&ab, IswJustifyRight);
    if (!btn_cb)
        IswArgBuilderAdd(&ab, IswNsensitive, (IswArgVal)False);
    Widget btn = IswCreateManagedWidget("btBtn", commandWidgetClass,
                                         row, ab.args, ab.count);
    if (btn_cb) {
        MenuAction *a = alloc_action(tb, d->path);
        IswAddCallback(btn, IswNcallback, btn_cb, a);
    }

    IswManageChild(row);
}

/* ---------- build listbox content ---------- */

static void build_content(TrayBt *tb)
{
    Widget listbox = tb->popup_listbox;

    if (!tb->bluez_available || !tb->has_adapter) {
        add_heading(listbox, "Bluetooth unavailable");
        return;
    }

    if (!tb->adapter.powered) {
        add_heading(listbox, "Bluetooth is off");
        return;
    }

    /* Connected devices */
    int has_connected = 0;
    for (int i = 0; i < tb->ndevices; i++) {
        if (tb->devices[i].connected) {
            if (!has_connected)
                add_heading(listbox, "Connected");
            has_connected = 1;
            if (tb->devices[i].busy)
                add_device_row(tb, listbox, &tb->devices[i],
                               "Disconnecting...", NULL);
            else
                add_device_row(tb, listbox, &tb->devices[i],
                               "Disconnect", on_disconnect);
        }
    }

    if (has_connected)
        add_heading(listbox, "");

    /* Paired but not connected */
    int has_paired = 0;
    for (int i = 0; i < tb->ndevices; i++) {
        if (tb->devices[i].paired && !tb->devices[i].connected) {
            if (!has_paired)
                add_heading(listbox, "Paired");
            has_paired = 1;
            if (tb->devices[i].busy)
                add_device_row(tb, listbox, &tb->devices[i],
                               "Connecting...", NULL);
            else
                add_device_row(tb, listbox, &tb->devices[i],
                               "Connect", on_connect);
        }
    }

    if (has_paired)
        add_heading(listbox, "");

    /* Available (not paired) */
    int has_available = 0;
    for (int i = 0; i < tb->ndevices; i++) {
        if (!tb->devices[i].paired) {
            if (!has_available)
                add_heading(listbox, "Available");
            has_available = 1;
            if (tb->devices[i].busy)
                add_device_row(tb, listbox, &tb->devices[i],
                               "Pairing...", NULL);
            else
                add_device_row(tb, listbox, &tb->devices[i],
                               "Pair", on_pair);
        }
    }

    if (!has_connected && !has_paired && !has_available) {
        if (tb->adapter.discovering)
            add_heading(listbox, "Scanning...");
        else
            add_heading(listbox, "No devices");
    }
}

/* ---------- public API ---------- */

void tb_menu_init(TrayBt *tb)
{
    tb->popup_shell = NULL;
    tb->popup_outer = NULL;
    tb->popup_viewport = NULL;
    tb->popup_listbox = NULL;
    tb->popup_visible = 0;
}

void tb_menu_show(TrayBt *tb)
{
    const IsdeColorScheme *scheme = isde_theme_current();

    if (tb->popup_visible) {
        tb_menu_hide(tb);
        return;
    }

    if (tb->popup_shell) {
        IswDestroyWidget(tb->popup_shell);
        tb->popup_shell = NULL;
        tb->popup_outer = NULL;
        tb->popup_viewport = NULL;
        tb->popup_listbox = NULL;
    }

    nactions = 0;

    IswArgBuilder ab = IswArgBuilderInit();

    /* Override shell */
    IswArgWidth(&ab, 300);
    IswArgHeight(&ab, 400);
    tb->popup_shell = IswCreatePopupShell("btPopup",
                                           overrideShellWidgetClass,
                                           tb->toplevel, ab.args, ab.count);

    /* Outer vertical FlexBox */
    IswArgBuilderReset(&ab);
    IswArgOrientation(&ab, IswOrientVertical);
    IswArgBorderWidth(&ab, 0);
    tb->popup_outer = IswCreateManagedWidget("outerBox", flexBoxWidgetClass,
                                              tb->popup_shell,
                                              ab.args, ab.count);

    /* Toggle row */
    IswArgBuilderReset(&ab);
    IswArgOrientation(&ab, IswOrientHorizontal);
    IswArgFlexBasis(&ab, 50);
    IswArgBorderWidth(&ab, 1);
    IswArgBackground(&ab, scheme->bg_light);
    Widget toggle_area = IswCreateManagedWidget("toggleArea", formWidgetClass,
                                                tb->popup_outer,
                                                ab.args, ab.count);

    /* Bluetooth power toggle */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Bluetooth");
    IswArgState(&ab, (tb->has_adapter && tb->adapter.powered) ? True : False);
    IswArgJustify(&ab, IswJustifyLeft);
    IswArgBackground(&ab, scheme->bg_light);
    Widget tw = IswCreateManagedWidget("btToggle", toggleWidgetClass,
                                        toggle_area, ab.args, ab.count);
    IswAddCallback(tw, IswNcallback, on_power_toggled, tb);

    /* Scan button */
    if (tb->has_adapter && tb->adapter.powered) {
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, tb->adapter.discovering ? "Stop Scan" : "Scan");
        IswArgJustify(&ab, IswJustifyRight);
        IswArgBackground(&ab, scheme->bg_light);
        IswArgBuilderAdd(&ab, IswNfromHoriz, (IswArgVal)tw);
        Widget scan_btn = IswCreateManagedWidget("scanBtn",
                                                  commandWidgetClass,
                                                  toggle_area,
                                                  ab.args, ab.count);
        IswAddCallback(scan_btn, IswNcallback, on_scan, tb);
    }

    /* Viewport — fills remaining space */
    IswArgBuilderReset(&ab);
    IswArgFlexGrow(&ab, 1);
    IswArgForceBars(&ab, True);
    IswArgBorderWidth(&ab, 0);
    IswArgBuilderAdd(&ab, IswNallowVert, (IswArgVal)True);
    IswArgBuilderAdd(&ab, IswNallowHoriz, (IswArgVal)False);
    IswArgBuilderAdd(&ab, IswNuseRight, (IswArgVal)True);
    tb->popup_viewport = IswCreateManagedWidget("viewport",
                                                 viewportWidgetClass,
                                                 tb->popup_outer,
                                                 ab.args, ab.count);

    /* ListBox inside viewport */
    IswArgBuilderReset(&ab);
    IswArgBuilderAdd(&ab, IswNselectionMode,
                     (IswArgVal)IswListBoxSelectNone);
    IswArgBuilderAdd(&ab, IswNshowSeparators, (IswArgVal)True);
    IswArgBuilderAdd(&ab, IswNrowSpacing, (IswArgVal)2);
    tb->popup_listbox = IswCreateManagedWidget("btList",
                                                listBoxWidgetClass,
                                                tb->popup_viewport,
                                                ab.args, ab.count);

    build_content(tb);

    IswRealizeWidget(tb->popup_shell);
    position_popup(tb);
    IswPopup(tb->popup_shell, IswGrabNone);

    {
        xcb_connection_t *conn = IswDisplay(tb->toplevel);
        xcb_grab_pointer(conn, True, IswWindow(tb->popup_shell),
                         XCB_EVENT_MASK_BUTTON_PRESS |
                         XCB_EVENT_MASK_BUTTON_RELEASE,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                         XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
        xcb_flush(conn);
    }

    IswAddEventHandler(tb->popup_shell, POPUP_DISMISS_MASK, False,
                       popup_outside_handler, tb);
    tb->popup_visible = 1;
}

void tb_menu_hide(TrayBt *tb)
{
    if (!tb->popup_visible)
        return;

    if (tb->popup_shell)
        IswRemoveEventHandler(tb->popup_shell, POPUP_DISMISS_MASK, False,
                              popup_outside_handler, tb);

    xcb_ungrab_pointer(IswDisplay(tb->toplevel), XCB_CURRENT_TIME);
    xcb_flush(IswDisplay(tb->toplevel));

    if (tb->popup_shell)
        IswPopdown(tb->popup_shell);

    tb->popup_visible = 0;
}

void tb_menu_rebuild(TrayBt *tb)
{
    if (in_action || !tb->popup_visible || !tb->popup_listbox)
        return;

    nactions = 0;

    /* Update toggle state */
    WidgetList outer_children;
    Cardinal outer_num;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgBuilderAdd(&ab, IswNchildren, (IswArgVal)&outer_children);
    IswArgBuilderAdd(&ab, IswNnumChildren, (IswArgVal)&outer_num);
    IswGetValues(tb->popup_outer, ab.args, ab.count);

    if (outer_num > 0) {
        Widget toggle_box = outer_children[0];
        WidgetList box_children;
        Cardinal box_num;
        IswArgBuilderReset(&ab);
        IswArgBuilderAdd(&ab, IswNchildren, (IswArgVal)&box_children);
        IswArgBuilderAdd(&ab, IswNnumChildren, (IswArgVal)&box_num);
        IswGetValues(toggle_box, ab.args, ab.count);

        if (box_num > 0) {
            IswArgBuilderReset(&ab);
            IswArgState(&ab, (tb->has_adapter && tb->adapter.powered)
                             ? True : False);
            IswSetValues(box_children[0], ab.args, ab.count);

            IswRemoveAllCallbacks(box_children[0], IswNcallback);
            IswAddCallback(box_children[0], IswNcallback,
                           on_power_toggled, tb);
        }

        if (box_num > 1) {
            IswArgBuilderReset(&ab);
            IswArgLabel(&ab, tb->adapter.discovering ? "Stop Scan" : "Scan");
            IswSetValues(box_children[1], ab.args, ab.count);
        }
    }

    clear_listbox(tb->popup_listbox);
    build_content(tb);
}

void tb_menu_cleanup(TrayBt *tb)
{
    if (tb->popup_shell) {
        IswDestroyWidget(tb->popup_shell);
        tb->popup_shell = NULL;
        tb->popup_outer = NULL;
        tb->popup_viewport = NULL;
        tb->popup_listbox = NULL;
    }
    tb->popup_visible = 0;

    free(actions);
    actions = NULL;
    nactions = 0;
    cap_actions = 0;
}
