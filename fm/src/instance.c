#define _POSIX_C_SOURCE 200809L
/*
 * instance.c — single-instance detection and path forwarding
 *
 * Uses an X selection (_ISDE_FM_INSTANCE) for instance detection.
 * If another instance owns the selection, sends the requested path
 * via a property + ClientMessage and returns.  The owning instance
 * watches for the message and opens a new window.
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

static xcb_atom_t atom_instance;   /* _ISDE_FM_INSTANCE */
static xcb_atom_t atom_open_path;  /* _ISDE_FM_OPEN_PATH */

static xcb_atom_t intern_atom(xcb_connection_t *conn, const char *name)
{
    xcb_intern_atom_cookie_t ck = xcb_intern_atom(conn, 0, strlen(name), name);
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(conn, ck, NULL);
    if (!r) { return XCB_ATOM_NONE; }
    xcb_atom_t a = r->atom;
    free(r);
    return a;
}

/* ---------- owning instance: handle incoming open-path request ---------- */

static void instance_event_handler(Widget w, IswPointer closure,
                                   xcb_generic_event_t *ev, Boolean *cont)
{
    (void)cont;
    FmApp *app = (FmApp *)closure;

    uint8_t type = ev->response_type & ~0x80;
    if (type != XCB_CLIENT_MESSAGE) {
        return;
    }

    xcb_client_message_event_t *cm = (xcb_client_message_event_t *)ev;
    if (cm->type != atom_open_path) {
        return;
    }

    /* Read the path from the _ISDE_FM_OPEN_PATH property on our window */
    xcb_connection_t *conn = IswDisplay(w);
    xcb_get_property_cookie_t cookie =
        xcb_get_property(conn, True, IswWindow(w),
                         atom_open_path, XCB_ATOM_STRING, 0, 4096);
    xcb_get_property_reply_t *reply = xcb_get_property_reply(conn, cookie, NULL);
    if (!reply) {
        return;
    }

    int len = xcb_get_property_value_length(reply);
    if (len > 0) {
        char *path = malloc(len + 1);
        memcpy(path, xcb_get_property_value(reply), len);
        path[len] = '\0';
        fm_window_new(app, path);
        free(path);
    }
    free(reply);
}

/* ---------- selection convert (we are the owner) ---------- */

static Boolean instance_convert(Widget w, Atom *selection, Atom *target,
                                Atom *type_return, IswPointer *value_return,
                                unsigned long *length_return, int *format_return)
{
    (void)selection;
    /* Return our window ID so the sender knows where to set the property */
    if (*target == atom_instance) {
        static xcb_window_t win_id;
        win_id = IswWindow(w);
        *type_return = XCB_ATOM_CARDINAL;
        *value_return = (IswPointer)&win_id;
        *length_return = 1;
        *format_return = 32;
        return True;
    }
    return False;
}

static void instance_lose(Widget w, Atom *selection)
{
    (void)w; (void)selection;
    /* Another instance took over — shouldn't happen, ignore */
}

/* ---------- public API ---------- */

/*
 * Returns: 1 if we are the primary instance (selection owned, handler installed)
 *          0 if another instance exists (path forwarded, caller should exit)
 *         -1 on error
 */
int instance_try_primary(FmApp *app, const char *path)
{
    xcb_connection_t *conn = IswDisplay(app->first_toplevel);
    Widget shell = app->first_toplevel;

    atom_instance  = intern_atom(conn, "_ISDE_FM_INSTANCE");
    atom_open_path = intern_atom(conn, "_ISDE_FM_OPEN_PATH");
    if (atom_instance == XCB_ATOM_NONE || atom_open_path == XCB_ATOM_NONE) {
        return -1;
    }

    /* The shell must be realized so it has a window */
    if (!IswIsRealized(shell)) {
        IswRealizeWidget(shell);
    }

    /* Try to own the selection */
    xcb_set_selection_owner(conn, IswWindow(shell), atom_instance,
                            XCB_CURRENT_TIME);
    xcb_flush(conn);

    /* Check if we got it */
    xcb_get_selection_owner_cookie_t ck =
        xcb_get_selection_owner(conn, atom_instance);
    xcb_get_selection_owner_reply_t *owner_reply =
        xcb_get_selection_owner_reply(conn, ck, NULL);
    if (!owner_reply) {
        return -1;
    }

    xcb_window_t owner = owner_reply->owner;
    free(owner_reply);

    if (owner == IswWindow(shell)) {
        /* We are the primary instance — listen for open-path messages */
        IswAddEventHandler(shell, (EventMask)0, True,
                          instance_event_handler, app);
        /* Also register the Xt selection convert so IswOwnSelection works
         * for future queries (not strictly needed for our protocol but
         * keeps things clean). */
        IswOwnSelection(shell, atom_instance, XCB_CURRENT_TIME,
                        instance_convert, instance_lose, NULL);
        return 1;
    }

    /* Another instance owns the selection — forward the path */
    /* Set the path as a property on the owner's window */
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, owner,
                        atom_open_path, XCB_ATOM_STRING, 8,
                        strlen(path), path);

    /* Send a ClientMessage to notify the owner */
    xcb_client_message_event_t cm;
    memset(&cm, 0, sizeof(cm));
    cm.response_type = XCB_CLIENT_MESSAGE;
    cm.window = owner;
    cm.type = atom_open_path;
    cm.format = 32;
    cm.data.data32[0] = IswWindow(shell);

    xcb_send_event(conn, False, owner, 0, (const char *)&cm);
    xcb_flush(conn);

    return 0;
}
