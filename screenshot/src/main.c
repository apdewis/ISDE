/*
 * isde-screenshot — screenshot tool for the ISDE desktop environment
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ISW/Shell.h>
#include <ISW/Cardinals.h>
#include <ISW/Form.h>
#include <ISW/FlexBox.h>
#include <ISW/Box.h>
#include <ISW/Label.h>
#include <ISW/Command.h>
#include <ISW/DrawingArea.h>
#include <ISW/IswArgMacros.h>

#include <isde/isde-theme.h>
#include <isde/isde-dbus.h>

#include "screenshot.h"

static Widget       toplevel;
static IswAppContext app;
static IsdeDBus    *dbus_conn;
static Widget       preview_w;
static Widget       save_btn;
static Screenshot  *current_capture;

#define PREVIEW_W  480
#define PREVIEW_H  320
#define BTN_W       80

/* ---------- theme reload ---------- */

static void
on_settings_changed(const char *section, const char *key, void *user_data)
{
    (void)key; (void)user_data;
    if (strcmp(section, "appearance") != 0 && strcmp(section, "*") != 0) {
        return;
    }
    isde_theme_reload();
    isde_theme_merge_xrm(toplevel);
    IswReloadResources(toplevel);
}

static void
dbus_input_cb(IswPointer client_data, int *fd, IswInputId *id)
{
    (void)fd; (void)id;
    IsdeDBus *bus = (IsdeDBus *)client_data;
    isde_dbus_dispatch(bus);
}

/* ---------- capture helpers ---------- */

static void
set_capture(Screenshot *ss)
{
    if (current_capture) {
        screenshot_free(current_capture);
    }
    current_capture = ss;
    preview_set_image(preview_w, current_capture);

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgSensitive(&ab, current_capture != NULL);
    IswSetValues(save_btn, ab.args, ab.count);
}

static xcb_atom_t atom_wm_change_state;
static xcb_atom_t atom_net_active_window;

static void
intern_atoms(xcb_connection_t *conn)
{
    if (atom_wm_change_state) {
        return;
    }
    xcb_intern_atom_cookie_t cc = xcb_intern_atom(conn, 0, 15, "WM_CHANGE_STATE");
    xcb_intern_atom_cookie_t ac = xcb_intern_atom(conn, 0, 18,
                                                   "_NET_ACTIVE_WINDOW");
    xcb_intern_atom_reply_t *cr = xcb_intern_atom_reply(conn, cc, NULL);
    xcb_intern_atom_reply_t *ar = xcb_intern_atom_reply(conn, ac, NULL);
    if (cr) { atom_wm_change_state = cr->atom; free(cr); }
    if (ar) { atom_net_active_window = ar->atom; free(ar); }
}

static void
send_minimize(xcb_connection_t *conn, xcb_screen_t *screen)
{
    xcb_client_message_event_t ev = {0};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = IswWindow(toplevel);
    ev.type = atom_wm_change_state;
    ev.data.data32[0] = 3; /* IconicState */

    xcb_send_event(conn, 0, screen->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                   XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   (const char *)&ev);
    xcb_flush(conn);
}

static void
send_restore(xcb_connection_t *conn, xcb_screen_t *screen)
{
    xcb_client_message_event_t ev = {0};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = IswWindow(toplevel);
    ev.type = atom_net_active_window;
    ev.data.data32[0] = 2; /* source = pager */

    xcb_send_event(conn, 0, screen->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                   XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   (const char *)&ev);
    xcb_flush(conn);
}

static xcb_window_t
get_wm_frame(xcb_connection_t *conn)
{
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(conn,
        xcb_query_tree(conn, IswWindow(toplevel)), NULL);
    if (!tree) {
        return IswWindow(toplevel);
    }
    xcb_window_t parent = tree->parent;
    xcb_window_t root = tree->root;
    free(tree);
    return (parent != root) ? parent : IswWindow(toplevel);
}

static void
wait_unmapped(xcb_connection_t *conn, xcb_window_t frame)
{
    for (int i = 0; i < 50; i++) {
        xcb_get_window_attributes_reply_t *attr =
            xcb_get_window_attributes_reply(conn,
                xcb_get_window_attributes(conn, frame), NULL);
        if (attr) {
            if (attr->map_state == XCB_MAP_STATE_UNMAPPED) {
                free(attr);
                return;
            }
            free(attr);
        }
        struct timespec ts = {0, 10000000};
        nanosleep(&ts, NULL);
    }
}

static void
do_capture(int fullscreen)
{
    xcb_connection_t *conn = IswDisplay(toplevel);
    const xcb_setup_t *setup = xcb_get_setup(conn);
    xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;

    intern_atoms(conn);

    xcb_window_t frame = get_wm_frame(conn);
    send_minimize(conn, screen);
    wait_unmapped(conn, frame);
    free(xcb_get_input_focus_reply(conn,
         xcb_get_input_focus(conn), NULL));

    Screenshot *ss;
    if (fullscreen) {
        ss = capture_fullscreen(conn, screen);
    } else {
        ss = capture_area(conn, screen);
    }

    send_restore(conn, screen);

    if (ss) {
        set_capture(ss);
    }
}

/* ---------- button callbacks ---------- */

static void
fullscreen_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)cd; (void)call;
    do_capture(1);
}

static void
area_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)cd; (void)call;
    do_capture(0);
}

static void
save_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)cd; (void)call;
    if (current_capture) {
        save_dialog(toplevel, current_capture);
    }
}

static void
do_quit(void)
{
    if (current_capture) {
        screenshot_free(current_capture);
    }
    if (dbus_conn) {
        isde_dbus_free(dbus_conn);
    }
    IswDestroyApplicationContext(app);
    exit(0);
}

static void
quit_action(Widget w, xcb_generic_event_t *ev, String *params, Cardinal *nparams)
{
    (void)w; (void)ev; (void)params; (void)nparams;
    do_quit();
}

/* ---------- UI construction ---------- */

static void
create_ui(Widget shell)
{
    IswArgBuilder ab = IswArgBuilderInit();

    /* Main vertical layout */
    IswArgOrientation(&ab, IswOrientVertical);
    IswArgSpacing(&ab, 8);
    Widget vbox = IswCreateManagedWidget("main", flexBoxWidgetClass,
                                         shell, ab.args, ab.count);

    /* Button row */
    IswArgBuilderReset(&ab);
    IswArgOrientation(&ab, IswOrientHorizontal);
    IswArgSpacing(&ab, 8);
    Widget btn_row = IswCreateManagedWidget("buttons", boxWidgetClass,
                                            vbox, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Full Screen");
    IswArgWidth(&ab, 100);
    IswArgInternalHeight(&ab, 8);
    IswArgInternalWidth(&ab, 8);
    Widget fs_btn = IswCreateManagedWidget("fullscreen", commandWidgetClass,
                                           btn_row, ab.args, ab.count);
    IswAddCallback(fs_btn, IswNcallback, fullscreen_cb, NULL);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Select Area");
    IswArgWidth(&ab, 100);
    IswArgInternalHeight(&ab, 8);
    IswArgInternalWidth(&ab, 8);
    Widget area_btn = IswCreateManagedWidget("area", commandWidgetClass,
                                             btn_row, ab.args, ab.count);
    IswAddCallback(area_btn, IswNcallback, area_cb, NULL);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Save");
    IswArgWidth(&ab, BTN_W);
    IswArgInternalHeight(&ab, 8);
    IswArgInternalWidth(&ab, 8);
    IswArgSensitive(&ab, False);
    save_btn = IswCreateManagedWidget("save", commandWidgetClass,
                                      btn_row, ab.args, ab.count);
    IswAddCallback(save_btn, IswNcallback, save_cb, NULL);

    /* Preview area — takes remaining vertical space */
    preview_w = preview_create(vbox, "preview");
    IswArgBuilderReset(&ab);
    IswArgFlexGrow(&ab, 1);
    IswSetValues(preview_w, ab.args, ab.count);
}

/* ---------- entry point ---------- */

int
main(int argc, char **argv)
{
    toplevel = IswAppInitialize(&app, "ISDE-Screenshot", NULL, 0,
                                &argc, argv, NULL, NULL, 0);
    isde_theme_merge_xrm(toplevel);

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgWidth(&ab, PREVIEW_W);
    IswArgHeight(&ab, PREVIEW_H + 50);
    IswArgMinWidth(&ab, 320);
    IswArgMinHeight(&ab, 240);
    IswArgInput(&ab, True);
    IswSetValues(toplevel, ab.args, ab.count);

    create_ui(toplevel);

    /* WM_DELETE_WINDOW */
    static IswActionsRec actions[] = {
        {"quit", quit_action},
    };
    IswAppAddActions(app, actions, IswNumber(actions));
    IswOverrideTranslations(toplevel,
        IswParseTranslationTable("<Message>WM_PROTOCOLS: quit()\n"));

    IswRealizeWidget(toplevel);

    /* Set WM_PROTOCOLS property */
    xcb_connection_t *conn = IswDisplay(toplevel);
    xcb_intern_atom_cookie_t wm_c = xcb_intern_atom(conn, 0, 16,
                                                     "WM_DELETE_WINDOW");
    xcb_intern_atom_cookie_t pr_c = xcb_intern_atom(conn, 0, 12,
                                                     "WM_PROTOCOLS");
    xcb_intern_atom_reply_t *wm_r = xcb_intern_atom_reply(conn, wm_c, NULL);
    xcb_intern_atom_reply_t *pr_r = xcb_intern_atom_reply(conn, pr_c, NULL);
    if (wm_r && pr_r) {
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE,
                            IswWindow(toplevel), pr_r->atom,
                            XCB_ATOM_ATOM, 32, 1, &wm_r->atom);
    }
    free(wm_r);
    free(pr_r);
    xcb_flush(conn);

    /* D-Bus theme watching */
    dbus_conn = isde_dbus_init();
    if (dbus_conn) {
        isde_dbus_settings_subscribe(dbus_conn, on_settings_changed, NULL);
        int dbus_fd = isde_dbus_get_fd(dbus_conn);
        if (dbus_fd >= 0) {
            IswAppAddInput(app, dbus_fd, (IswPointer)IswInputReadMask,
                          dbus_input_cb, dbus_conn);
        }
    }

    IswAppMainLoop(app);

    return 0;
}
