#define _POSIX_C_SOURCE 200809L
/*
 * daemon.c — FM-side D-Bus glue for the background daemon.
 *
 * Builds the per-display well-known D-Bus name, owns it, and registers
 * the OpenPath method handler that spawns windows on demand.  All
 * FM-specific D-Bus logic lives here; platform/common/dbus.c only
 * provides the generic session-bus connection.
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---------- FM D-Bus constants ---------- */

#define FM_DBUS_PATH        "/org/isde/FileManager"
#define FM_DBUS_INTERFACE   "org.isde.FileManager"
#define FM_DBUS_BASE_NAME   "org.isde.FileManager"

/* ---------- per-display well-known name ---------- */

const char *fm_dbus_name(void)
{
    static char buf[256];
    const char *display = getenv("DISPLAY");
    if (!display || !display[0]) {
        display = ":0";
    }

    snprintf(buf, sizeof(buf), "%s.", FM_DBUS_BASE_NAME);
    size_t off = strlen(buf);
    for (const char *p = display; *p && off < sizeof(buf) - 1; p++) {
        buf[off++] = isalnum((unsigned char)*p) ? *p : '_';
    }
    buf[off] = '\0';
    return buf;
}

/* ---------- OpenPath method handler ---------- */

static void strip_file_uri(char *path)
{
    if (strncmp(path, "file://", 7) == 0) {
        size_t rest = strlen(path + 7) + 1;
        memmove(path, path + 7, rest);
        if (path[0] == '\0') {
            strcpy(path, "/");
        }
    }
}

static DBusHandlerResult
open_path_message_cb(DBusConnection *conn, DBusMessage *msg,
                     void *user_data)
{
    (void)conn;
    FmApp *app = (FmApp *)user_data;
    DBusConnection *reply_conn = (DBusConnection *)isde_dbus_get_connection(app->dbus);

    if (!dbus_message_is_method_call(msg, FM_DBUS_INTERFACE, "OpenPath")) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const char *path = NULL;
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err,
                               DBUS_TYPE_STRING, &path,
                               DBUS_TYPE_INVALID)) {
        if (reply_conn) {
            DBusMessage *reply = dbus_message_new_error(
                msg, DBUS_ERROR_INVALID_ARGS, "Expected a string path");
            dbus_connection_send(reply_conn, reply, NULL);
            dbus_connection_flush(reply_conn);
            dbus_message_unref(reply);
        }
        dbus_error_free(&err);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    char *path_copy = strdup(path ? path : "/");
    if (path_copy) {
        strip_file_uri(path_copy);
        fm_window_new(app, path_copy);
        free(path_copy);
    }

    if (reply_conn) {
        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_connection_send(reply_conn, reply, NULL);
        dbus_connection_flush(reply_conn);
        dbus_message_unref(reply);
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* ---------- name ownership ---------- */

static int fm_own_name(IsdeDBus *bus, const char *name)
{
    DBusConnection *conn = (DBusConnection *)isde_dbus_get_connection(bus);
    if (!conn || !name) { return -1; }

    DBusError err;
    dbus_error_init(&err);
    int result = dbus_bus_request_name(conn, name,
                                       DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "isde-fm: request_name(%s) failed: %s\n",
                name, err.message ? err.message : "(unknown)");
        dbus_error_free(&err);
        return -1;
    }
    return (result == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) ? 1 : 0;
}

int fm_name_has_owner(IsdeDBus *bus, const char *name)
{
    DBusConnection *conn = (DBusConnection *)isde_dbus_get_connection(bus);
    if (!conn || !name) { return 0; }

    DBusError err;
    dbus_error_init(&err);
    dbus_bool_t exists = dbus_bus_name_has_owner(conn, name, &err);
    if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return 0;
    }
    return exists ? 1 : 0;
}

/* ---------- OpenPath call (launcher side) ---------- */

int fm_call_open_path(IsdeDBus *bus, const char *service,
                      const char *path_str)
{
    DBusConnection *conn = (DBusConnection *)isde_dbus_get_connection(bus);
    if (!conn || !service || !path_str) { return -1; }

    DBusMessage *msg = dbus_message_new_method_call(service,
                                                    FM_DBUS_PATH,
                                                    FM_DBUS_INTERFACE,
                                                    "OpenPath");
    if (!msg) { return -1; }

    const char *p = path_str;
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &p, DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        conn, msg, 5000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        fprintf(stderr, "isde-fm: OpenPath call to %s failed: %s\n",
                service, err.message ? err.message : "(unknown)");
        dbus_error_free(&err);
        return -1;
    }
    dbus_message_unref(reply);
    dbus_error_free(&err);
    return 0;
}

/* ---------- registration ---------- */

int fm_daemon_register(FmApp *app)
{
    if (!app || !app->dbus) {
        return -1;
    }

    const char *name = fm_dbus_name();
    int rc = fm_own_name(app->dbus, name);
    if (rc <= 0) {
        /* 0: another daemon owns the name; -1: error. */
        return rc;
    }

    DBusConnection *conn = (DBusConnection *)isde_dbus_get_connection(app->dbus);
    if (!conn) {
        return -1;
    }

    DBusObjectPathVTable vtable;
    memset(&vtable, 0, sizeof(vtable));
    vtable.message_function = open_path_message_cb;

    if (!dbus_connection_register_object_path(conn, FM_DBUS_PATH,
                                               &vtable, app)) {
        return -1;
    }
    return 1;
}
