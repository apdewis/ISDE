#define _POSIX_C_SOURCE 200809L
/*
 * tray-net-menu.c — popup for the network tray module
 */
#include "tray-net.h"

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
    TrayNet *tn = (TrayNet *)client_data;
    if (event->kind != IswButtonDown)
        return;
    panel_dismiss_popup(tn->panel);
    tn->popup_visible = 0;
}

/* ---------- callback data ---------- */

typedef struct MenuAction {
    TrayNet    *tn;
    char        path[TN_PATH_LEN];
} MenuAction;

typedef struct ToggleData {
    TrayNet    *tn;
    char        path[TN_PATH_LEN];
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

/* ---------- position popup above the tray icon ---------- */

static void position_popup(TrayNet *tn)
{
    Panel *p = tn->panel;
    Widget popup = tn->popup_shell;

    if (!popup || !tn->icon)
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
    Widget w = tn->icon;
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

/* ---------- add a network service row ---------- */

static void add_service_row(TrayNet *tn, Widget listbox,
                            const ServiceInfo *s, int connected)
{
    IswArgBuilder ab = IswArgBuilderInit();

    char display[512];

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
    Panel *p = tn->panel;
    const IsdeColorScheme *scheme = isde_theme_current();

    if (tn->popup_visible) {
        tn_menu_hide(tn);
        return;
    }

    nactions = 0;
    ntoggles = 0;

    if (!tn->popup_shell) {
        IswArgBuilder ab = IswArgBuilderInit();

        IswArgWidth(&ab, 400);
        IswArgHeight(&ab, 400);
        tn->popup_shell = IswCreatePopupShell("netPopup",
                                               overrideShellWidgetClass,
                                               p->toplevel, ab.args, ab.count);
        IswAddEventHandler(tn->popup_shell, IswButtonPressMask, False,
                           popup_button_handler, tn);

        /* Outer vertical FlexBox */
        IswArgBuilderReset(&ab);
        IswArgOrientation(&ab, IswOrientVertical);
        IswArgBorderWidth(&ab, 0);
        tn->popup_outer = IswCreateManagedWidget("outerBox", flexBoxWidgetClass,
                                                  tn->popup_shell,
                                                  ab.args, ab.count);

        /* Toggle row */
        IswArgBuilderReset(&ab);
        IswArgOrientation(&ab, IswOrientHorizontal);
        IswArgFlexBasis(&ab, 50);
        IswArgBorderBottom(&ab, 1);
        if (scheme)
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
            if (scheme)
                IswArgBackground(&ab, scheme->bg_light);
            Widget tw = IswCreateManagedWidget("techToggle", toggleWidgetClass,
                                                toggle_area, ab.args, ab.count);
            IswAddCallback(tw, IswNcallback, on_tech_toggled, td);
        }

        /* Viewport */
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

        /* ListBox */
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
    } else {
        tn_menu_rebuild(tn);
    }

    position_popup(tn);
    IswPopup(tn->popup_shell, IswGrabNone);
    IswGrabPointer(tn->popup_shell, True,
                   IswButtonPressMask | IswButtonReleaseMask,
                   IswCursorNone, ISW_CURRENT_TIME);

    panel_show_popup(p, tn->popup_shell);
    tn->popup_visible = 1;
}

void tn_menu_hide(TrayNet *tn)
{
    if (!tn->popup_visible)
        return;

    panel_dismiss_popup(tn->panel);
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
