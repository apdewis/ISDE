#define _POSIX_C_SOURCE 200809L
/*
 * instance.c — single-instance detection and path forwarding
 *
 * Uses an X selection (_ISDE_FM_INSTANCE) for instance detection.
 * If another instance owns the selection, sends the requested path
 * via a property + ClientMessage and returns.  The owning instance
 * watches for the message and opens a new window.
 *
 * This module uses native XCB handles via IswDisplayNativeHandle()
 * because single-instance coordination is an X11-protocol operation
 * (selection ownership, properties, client messages) with no neutral
 * ISW equivalent.
 */
#include "fm.h"

#include <ISW/ISWPlatform.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

static Atom atom_instance;   /* _ISDE_FM_INSTANCE */
static Atom atom_open_path;  /* _ISDE_FM_OPEN_PATH */

/* ---------- owning instance: handle incoming open-path request ---------- */

static void instance_event_handler(Widget w, IswPointer closure,
                                   IswEvent *ev, Boolean *cont)
{
    (void)cont;
    FmApp *app = (FmApp *)closure;

    if (ev->kind != IswProtocol)
        return;

    if (ev->protocol.message_type != (IswProtocolId)atom_open_path)
        return;

    /* Read the path from the _ISDE_FM_OPEN_PATH property on our window */
    xcb_connection_t *conn =
        (xcb_connection_t *)IswDisplayNativeHandle(IswDisplayOf(w));
    xcb_window_t win = (xcb_window_t)(uintptr_t)IswWindowNativeHandle(
        _IswPlatformWidgetWindow(IswDisplayOf(w), w));
    xcb_get_property_cookie_t cookie =
        xcb_get_property(conn, True, win,
                         (xcb_atom_t)atom_open_path, XCB_ATOM_STRING, 0, 4096);
    xcb_get_property_reply_t *reply = xcb_get_property_reply(conn, cookie, NULL);
    if (!reply) {
        return;
    }

    int len = xcb_get_property_value_length(reply);
    if (len > 0) {
        char *path = malloc(len + 1);
        memcpy(path, xcb_get_property_value(reply), len);
        path[len] = '\0';
        const char *open_path = path;
        if (strncmp(open_path, "file://", 7) == 0) {
            open_path += 7;
            if (open_path[0] == '\0')
                open_path = "/";
        }
        fm_window_new(app, open_path);
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
        static uint32_t win_id;
        win_id = (uint32_t)(uintptr_t)IswWindowNativeHandle(
            _IswPlatformWidgetWindow(IswDisplayOf(w), w));
        *type_return = IswDndInternType(w, "CARDINAL");
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
}

/* ---------- public API ---------- */

/*
 * Returns: 1 if we are the primary instance (selection owned, handler installed)
 *          0 if another instance exists (path forwarded, caller should exit)
 *         -1 on error
 */
int instance_try_primary(FmApp *app, const char *path)
{
    xcb_connection_t *conn =
        (xcb_connection_t *)IswDisplayNativeHandle(IswDisplayOf(app->first_toplevel));
    Widget shell = app->first_toplevel;

    atom_instance  = IswDndInternType(shell, "_ISDE_FM_INSTANCE");
    atom_open_path = IswDndInternType(shell, "_ISDE_FM_OPEN_PATH");
    if (atom_instance == None || atom_open_path == None) {
        return -1;
    }

    /* The shell must be realized so it has a window */
    if (!IswIsRealized(shell)) {
        IswRealizeWidget(shell);
    }

    xcb_window_t our_win = (xcb_window_t)(uintptr_t)IswWindowNativeHandle(
        _IswPlatformWidgetWindow(IswDisplayOf(shell), shell));

    /* Check if another instance already owns the selection */
    xcb_get_selection_owner_cookie_t ck =
        xcb_get_selection_owner(conn, (xcb_atom_t)atom_instance);
    xcb_get_selection_owner_reply_t *owner_reply =
        xcb_get_selection_owner_reply(conn, ck, NULL);
    if (!owner_reply) {
        return -1;
    }

    xcb_window_t owner = owner_reply->owner;
    free(owner_reply);

    if (owner == XCB_WINDOW_NONE) {
        /* No existing instance — claim ownership */
        xcb_set_selection_owner(conn, our_win, (xcb_atom_t)atom_instance,
                                XCB_CURRENT_TIME);
        xcb_flush(conn);

        /* Verify we actually got it (another instance could race us) */
        ck = xcb_get_selection_owner(conn, (xcb_atom_t)atom_instance);
        owner_reply = xcb_get_selection_owner_reply(conn, ck, NULL);
        if (!owner_reply) {
            return -1;
        }
        owner = owner_reply->owner;
        free(owner_reply);

        if (owner != our_win) {
            goto forward;
        }

        /* We are the primary instance — listen for open-path messages */
        IswAddEventHandler(shell, (EventMask)0, True,
                          instance_event_handler, app);
        IswOwnSelection(shell, atom_instance, 0 /* CurrentTime */,
                        instance_convert, instance_lose, NULL);
        return 1;
    }

forward:
    /* Another instance owns the selection — forward the path */
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, owner,
                        (xcb_atom_t)atom_open_path, XCB_ATOM_STRING, 8,
                        strlen(path), path);

    xcb_client_message_event_t cm;
    memset(&cm, 0, sizeof(cm));
    cm.response_type = XCB_CLIENT_MESSAGE;
    cm.window = owner;
    cm.type = (xcb_atom_t)atom_open_path;
    cm.format = 32;
    cm.data.data32[0] = our_win;

    xcb_send_event(conn, False, owner, 0, (const char *)&cm);
    xcb_flush(conn);

    return 0;
}
