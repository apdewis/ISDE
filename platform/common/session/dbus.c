#include <dbus/dbus.h>
#include <stdio.h>
#include <string.h>
#include "session.h"

#define SCREENSAVER_DBUS_IFACE "org.freedesktop.ScreenSaver"
#define SCREENSAVER_DBUS_PATH  "/org/freedesktop/ScreenSaver"
#define SCREENSAVER_DBUS_NAME  "org.freedesktop.ScreenSaver"

static const DBusObjectPathVTable screensaver_vtable = {
    .message_function = screensaver_message_handler
};

/* Drop any inhibitors held by a bus name that has vanished (e.g. a browser
 * that crashed without calling UnInhibit), so blanking is not wedged off. */
DBusHandlerResult
session_bus_filter(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    (void)conn;
    Session *s = (Session *)user_data;

    if (dbus_message_is_signal(msg, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
        const char *name = NULL;
        const char *old_owner = NULL;
        const char *new_owner = NULL;
        if (!dbus_message_get_args(msg, NULL,
                                   DBUS_TYPE_STRING, &name,
                                   DBUS_TYPE_STRING, &old_owner,
                                   DBUS_TYPE_STRING, &new_owner,
                                   DBUS_TYPE_INVALID)) {
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        if (new_owner && new_owner[0] != '\0') {
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;  /* name acquired */
        }

        int removed = 0;
        for (int i = 0; i < s->inhibit_count; ) {
            if (name && strcmp(s->inhibit_owners[i], name) == 0) {
                fprintf(stderr, "isde-session: dropping inhibitor cookie %u "
                        "(owner '%s' gone)\n", s->inhibit_cookies[i], name);
                int last = --s->inhibit_count;
                s->inhibit_cookies[i] = s->inhibit_cookies[last];
                memcpy(s->inhibit_owners[i], s->inhibit_owners[last],
                       sizeof(s->inhibit_owners[i]));
                removed = 1;
            } else {
                i++;
            }
        }
        if (removed) {
            update_blanking_inhibit(s);
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void init_screensaver_service(Session *s)
{
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!conn) {
        fprintf(stderr, "isde-session: screensaver service: %s\n",
                err.message ? err.message : "cannot connect");
        dbus_error_free(&err);
        return;
    }

    int ret = dbus_bus_request_name(conn, SCREENSAVER_DBUS_NAME,
                                    DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "isde-session: cannot own %s: %s\n",
                SCREENSAVER_DBUS_NAME,
                dbus_error_is_set(&err) ? err.message : "already owned");
        dbus_error_free(&err);
        dbus_connection_unref(conn);
        return;
    }

    dbus_connection_set_exit_on_disconnect(conn, FALSE);

    dbus_connection_register_object_path(conn, SCREENSAVER_DBUS_PATH,
                                         &screensaver_vtable, s);
    dbus_connection_register_object_path(conn, "/ScreenSaver",
                                         &screensaver_vtable, s);

    /* Watch for inhibitor owners disconnecting (name-lost only). */
    dbus_connection_add_filter(conn, session_bus_filter, s, NULL);
    dbus_bus_add_match(conn,
        "type='signal',"
        "interface='" DBUS_INTERFACE_DBUS "',"
        "member='NameOwnerChanged',"
        "arg2=''",
        &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "isde-session: D-Bus match (owner): %s\n", err.message);
        dbus_error_free(&err);
    }

    s->session_bus = conn;
    fprintf(stderr, "isde-session: screensaver inhibit service active\n");
}

/* ---------- org.freedesktop.ScreenSaver inhibit ---------- */



DBusHandlerResult
screensaver_message_handler(DBusConnection *conn, DBusMessage *msg,
                            void *user_data)
{
    Session *s = (Session *)user_data;

    if (dbus_message_is_method_call(msg, SCREENSAVER_DBUS_IFACE, "Inhibit")) {
        const char *app = NULL;
        const char *reason = NULL;
        if (!dbus_message_get_args(msg, NULL,
                                   DBUS_TYPE_STRING, &app,
                                   DBUS_TYPE_STRING, &reason,
                                   DBUS_TYPE_INVALID)) {
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        if (s->inhibit_count >= 32) {
            DBusMessage *err = dbus_message_new_error(msg,
                DBUS_ERROR_LIMITS_EXCEEDED, "too many inhibitors");
            dbus_connection_send(conn, err, NULL);
            dbus_message_unref(err);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        uint32_t cookie = ++s->next_cookie;
        const char *sender = dbus_message_get_sender(msg);
        int idx = s->inhibit_count++;
        s->inhibit_cookies[idx] = cookie;
        snprintf(s->inhibit_owners[idx], sizeof(s->inhibit_owners[idx]),
                 "%s", sender ? sender : "");
        fprintf(stderr, "isde-session: screensaver inhibit by '%s': %s "
                "(cookie %u, active %d)\n", app, reason, cookie,
                s->inhibit_count);
        update_blanking_inhibit(s);

        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_message_append_args(reply,
                                 DBUS_TYPE_UINT32, &cookie,
                                 DBUS_TYPE_INVALID);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, SCREENSAVER_DBUS_IFACE, "UnInhibit")) {
        uint32_t cookie = 0;
        if (!dbus_message_get_args(msg, NULL,
                                   DBUS_TYPE_UINT32, &cookie,
                                   DBUS_TYPE_INVALID)) {
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        for (int i = 0; i < s->inhibit_count; i++) {
            if (s->inhibit_cookies[i] == cookie) {
                int last = --s->inhibit_count;
                s->inhibit_cookies[i] = s->inhibit_cookies[last];
                memcpy(s->inhibit_owners[i], s->inhibit_owners[last],
                       sizeof(s->inhibit_owners[i]));
                fprintf(stderr, "isde-session: screensaver uninhibit "
                        "cookie %u (active %d)\n", cookie,
                        s->inhibit_count);
                update_blanking_inhibit(s);
                break;
            }
        }
        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ---------- DM D-Bus helper ---------- */

void dm_dbus_call(const char *method)
{
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!conn) {
        fprintf(stderr, "isde-session: D-Bus: %s\n",
                err.message ? err.message : "cannot connect");
        dbus_error_free(&err);
        return;
    }

    DBusMessage *msg = dbus_message_new_method_call(
        "org.isde.DisplayManager",
        "/org/isde/DisplayManager",
        "org.isde.DisplayManager",
        method);
    if (msg) {
        dbus_connection_send(conn, msg, NULL);
        dbus_connection_flush(conn);
        dbus_message_unref(msg);
    }
    dbus_connection_unref(conn);
    dbus_error_free(&err);
}