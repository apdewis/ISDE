#define _POSIX_C_SOURCE 200809L
/*
 * tray-bt-menu.c — popup for the bluetooth tray module
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- callback data ---------- */

typedef struct MenuAction {
    TrayBt     *tb;
    char        path[TB_PATH_LEN];
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
    BtDeviceInfo *dev = tb_bluez_find_device(a->tb, a->path);
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
    BtDeviceInfo *dev = tb_bluez_find_device(a->tb, a->path);
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
    BtDeviceInfo *dev = tb_bluez_find_device(a->tb, a->path);
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

/* ---------- helpers ---------- */

static const char *device_icon_name(const BtDeviceInfo *d)
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

static const char *device_display_name(const BtDeviceInfo *d)
{
    if (d->name[0] != '\0')
        return d->name;
    if (d->address[0] != '\0')
        return d->address;
    return "(unknown)";
}

/* ---------- position popup above the tray icon ---------- */

static void position_popup(TrayBt *tb)
{
    Panel *p = tb->panel;
    Widget popup = tb->popup_shell;

    if (!popup || !tb->icon)
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
    Widget w = tb->icon;
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
    IswArgSelectable(&ab, False);
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
                           const BtDeviceInfo *d, const char *btn_label,
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
    Panel *p = tb->panel;
    const IsdeColorScheme *scheme = isde_theme_current();

    if (tb->popup_visible) {
        tb_menu_hide(tb);
        return;
    }

    nactions = 0;

    if (!tb->popup_shell) {
        IswArgBuilder ab = IswArgBuilderInit();

        /* Override shell */
        IswArgWidth(&ab, 300);
        IswArgHeight(&ab, 400);
        tb->popup_shell = IswCreatePopupShell("btPopup",
                                               overrideShellWidgetClass,
                                               p->toplevel, ab.args, ab.count);

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
        IswArgBorderBottom(&ab, 1);
        if (scheme)
            IswArgBackground(&ab, scheme->bg_light);
        Widget toggle_area = IswCreateManagedWidget("toggleArea", formWidgetClass,
                                                    tb->popup_outer,
                                                    ab.args, ab.count);

        /* Bluetooth power toggle */
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, "Bluetooth");
        IswArgState(&ab, (tb->has_adapter && tb->adapter.powered) ? True : False);
        IswArgJustify(&ab, IswJustifyLeft);
        if (scheme)
            IswArgBackground(&ab, scheme->bg_light);
        Widget tw = IswCreateManagedWidget("btToggle", toggleWidgetClass,
                                            toggle_area, ab.args, ab.count);
        IswAddCallback(tw, IswNcallback, on_power_toggled, tb);

        /* Scan button */
        if (tb->has_adapter && tb->adapter.powered) {
            IswArgBuilderReset(&ab);
            IswArgLabel(&ab, tb->adapter.discovering ? "Stop Scan" : "Scan");
            IswArgJustify(&ab, IswJustifyRight);
            if (scheme)
                IswArgBackground(&ab, scheme->bg_light);
            IswArgBuilderAdd(&ab, IswNfromHoriz, (IswArgVal)tw);
            Widget scan_btn = IswCreateManagedWidget("scanBtn",
                                                    commandWidgetClass,
                                                    toggle_area,
                                                    ab.args, ab.count);
            IswAddCallback(scan_btn, IswNcallback, on_scan, tb);
        }

        /* Viewport -- fills remaining space */
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
    } else {
        tb_menu_rebuild(tb);
    }

    position_popup(tb);
    IswPopup(tb->popup_shell, IswGrabNone);
    IswGrabPointer(tb->popup_shell, True,
                   IswButtonPressMask | IswButtonReleaseMask,
                   XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                   None, IswCursorNone, ISW_CURRENT_TIME);

    panel_show_popup(p, tb->popup_shell);
    tb->popup_visible = 1;
}

void tb_menu_hide(TrayBt *tb)
{
    if (!tb->popup_visible)
        return;

    panel_dismiss_popup(tb->panel);
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
