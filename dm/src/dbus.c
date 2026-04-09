#define _POSIX_C_SOURCE 200809L
/*
 * dbus.c — D-Bus system bus interface for isde-dm
 *
 * Bus name:  org.isde.DisplayManager
 * Object:    /org/isde/DisplayManager
 * Interface: org.isde.DisplayManager
 *
 * Methods:  Lock, Shutdown, Reboot, Suspend, ShowConfirmation,
 *           GetGreeterConfig, SetGreeterConfig
 * Signals:  SessionStarted, SessionEnded, Locked, Unlocked,
 *           ConfirmationRequested, GreeterConfigChanged
 */
#include "dm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dbus/dbus.h>

#include "isde/isde-config.h"

#define DM_DBUS_SERVICE   "org.isde.DisplayManager"
#define DM_DBUS_PATH      "/org/isde/DisplayManager"
#define DM_DBUS_INTERFACE "org.isde.DisplayManager"

/* Forward declarations for lock/unlock (implemented in dm.c or ipc.c) */
void dm_lock_session(Dm *dm);

static DBusHandlerResult
handle_method(DBusConnection *conn, DBusMessage *msg, void *userdata)
{
    Dm *dm = (Dm *)userdata;

    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const char *iface = dbus_message_get_interface(msg);
    const char *method = dbus_message_get_member(msg);
    if (!method) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    /* Introspection */
    if (iface && strcmp(iface, "org.freedesktop.DBus.Introspectable") == 0 &&
        strcmp(method, "Introspect") == 0) {
        const char *xml =
            "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object "
            "Introspection 1.0//EN\" \"http://www.freedesktop.org/standards/"
            "dbus/1.0/introspect.dtd\">\n"
            "<node>\n"
            "  <interface name=\"" DM_DBUS_INTERFACE "\">\n"
            "    <method name=\"Lock\"/>\n"
            "    <method name=\"Shutdown\"/>\n"
            "    <method name=\"Reboot\"/>\n"
            "    <method name=\"Suspend\"/>\n"
            "    <method name=\"ShowConfirmation\">\n"
            "      <arg name=\"action\" type=\"s\" direction=\"in\"/>\n"
            "    </method>\n"
            "    <method name=\"GetGreeterConfig\">\n"
            "      <arg name=\"config\" type=\"a{ss}\" direction=\"out\"/>\n"
            "    </method>\n"
            "    <method name=\"SetGreeterConfig\">\n"
            "      <arg name=\"key\" type=\"s\" direction=\"in\"/>\n"
            "      <arg name=\"value\" type=\"s\" direction=\"in\"/>\n"
            "    </method>\n"
            "    <signal name=\"SessionStarted\">\n"
            "      <arg name=\"username\" type=\"s\"/>\n"
            "      <arg name=\"session\" type=\"s\"/>\n"
            "    </signal>\n"
            "    <signal name=\"SessionEnded\">\n"
            "      <arg name=\"username\" type=\"s\"/>\n"
            "    </signal>\n"
            "    <signal name=\"Locked\"/>\n"
            "    <signal name=\"Unlocked\"/>\n"
            "    <signal name=\"ConfirmationRequested\">\n"
            "      <arg name=\"action\" type=\"s\"/>\n"
            "    </signal>\n"
            "    <signal name=\"GreeterConfigChanged\">\n"
            "      <arg name=\"key\" type=\"s\"/>\n"
            "    </signal>\n"
            "  </interface>\n"
            "</node>\n";

        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_message_append_args(reply,
                                 DBUS_TYPE_STRING, &xml,
                                 DBUS_TYPE_INVALID);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* Only handle our interface */
    if (!iface || strcmp(iface, DM_DBUS_INTERFACE) != 0) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    DBusMessage *reply = NULL;

    fprintf(stderr, "isde-dm: D-Bus method: %s (iface=%s)\n",
            method, iface ? iface : "(null)");

    if (strcmp(method, "Lock") == 0) {
        dm_lock_session(dm);
        reply = dbus_message_new_method_return(msg);
    } else if (strcmp(method, "Shutdown") == 0) {
        if (dm->allow_shutdown) {
            dm_power_shutdown(dm);
        }
        reply = dbus_message_new_method_return(msg);
    } else if (strcmp(method, "Reboot") == 0) {
        if (dm->allow_reboot) {
            dm_power_reboot(dm);
        }
        reply = dbus_message_new_method_return(msg);
    } else if (strcmp(method, "Suspend") == 0) {
        if (dm->allow_suspend) {
            dm_power_suspend(dm);
        }
        reply = dbus_message_new_method_return(msg);
    } else if (strcmp(method, "ShowConfirmation") == 0) {
        const char *action = NULL;
        if (dbus_message_get_args(msg, NULL,
                                  DBUS_TYPE_STRING, &action,
                                  DBUS_TYPE_INVALID) && action) {
            /* Validate action */
            if (strcmp(action, "shutdown") == 0 ||
                strcmp(action, "reboot") == 0 ||
                strcmp(action, "suspend") == 0 ||
                strcmp(action, "logout") == 0) {
                /* Emit ConfirmationRequested signal for session to pick up */
                DBusMessage *sig = dbus_message_new_signal(
                    DM_DBUS_PATH, DM_DBUS_INTERFACE, "ConfirmationRequested");
                if (sig) {
                    dbus_message_append_args(sig,
                                             DBUS_TYPE_STRING, &action,
                                             DBUS_TYPE_INVALID);
                    dbus_connection_send(conn, sig, NULL);
                    dbus_message_unref(sig);
                    dbus_connection_flush(conn);
                }
            }
        }
        reply = dbus_message_new_method_return(msg);
    } else if (strcmp(method, "GetGreeterConfig") == 0) {
        reply = dbus_message_new_method_return(msg);
        DBusMessageIter iter, dict;
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{ss}", &dict);

        const char *keys[] = { "clock_time_format", "clock_date_format" };
        const char *vals[] = { dm->clock_time_fmt, dm->clock_date_fmt };
        for (int i = 0; i < 2; i++) {
            DBusMessageIter entry;
            dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY,
                                             NULL, &entry);
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &keys[i]);
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &vals[i]);
            dbus_message_iter_close_container(&dict, &entry);
        }

        dbus_message_iter_close_container(&iter, &dict);
    } else if (strcmp(method, "SetGreeterConfig") == 0) {
        const char *key = NULL;
        const char *value = NULL;
        if (dbus_message_get_args(msg, NULL,
                                  DBUS_TYPE_STRING, &key,
                                  DBUS_TYPE_STRING, &value,
                                  DBUS_TYPE_INVALID) && key && value) {
            int ok = 0;
            if (strcmp(key, "clock_time_format") == 0) {
                free(dm->clock_time_fmt);
                dm->clock_time_fmt = strdup(value);
                isde_config_write_string(DM_CONFIG_PATH, "clock",
                                         "time_format", value);
                ok = 1;
            } else if (strcmp(key, "clock_date_format") == 0) {
                free(dm->clock_date_fmt);
                dm->clock_date_fmt = strdup(value);
                isde_config_write_string(DM_CONFIG_PATH, "clock",
                                         "date_format", value);
                ok = 1;
            }

            if (ok) {
                /* Emit GreeterConfigChanged signal */
                DBusMessage *sig = dbus_message_new_signal(
                    DM_DBUS_PATH, DM_DBUS_INTERFACE, "GreeterConfigChanged");
                if (sig) {
                    dbus_message_append_args(sig,
                                             DBUS_TYPE_STRING, &key,
                                             DBUS_TYPE_INVALID);
                    dbus_connection_send(conn, sig, NULL);
                    dbus_message_unref(sig);
                    dbus_connection_flush(conn);
                }
            }
            reply = dbus_message_new_method_return(msg);
        } else {
            reply = dbus_message_new_error(msg,
                DBUS_ERROR_INVALID_ARGS, "Expected (key: s, value: s)");
        }
    } else {
        reply = dbus_message_new_error(msg,
            DBUS_ERROR_UNKNOWN_METHOD, "Unknown method");
    }

    if (reply) {
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}

static const DBusObjectPathVTable vtable = {
    .unregister_function = NULL,
    .message_function    = handle_method,
};

int dm_dbus_init(Dm *dm)
{
    DBusError err;
    dbus_error_init(&err);

    dm->dbus = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!dm->dbus) {
        fprintf(stderr, "isde-dm: D-Bus: %s\n",
                err.message ? err.message : "cannot connect");
        dbus_error_free(&err);
        return -1;
    }

    /* Don't exit the process if the bus disconnects */
    dbus_connection_set_exit_on_disconnect(dm->dbus, FALSE);

    int ret = dbus_bus_request_name(dm->dbus, DM_DBUS_SERVICE,
                                    DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "isde-dm: D-Bus: cannot acquire %s: %s\n",
                DM_DBUS_SERVICE,
                err.message ? err.message : "name already owned");
        dbus_error_free(&err);
        dbus_connection_unref(dm->dbus);
        dm->dbus = NULL;
        return -1;
    }

    dbus_connection_register_object_path(dm->dbus, DM_DBUS_PATH,
                                         &vtable, dm);

    fprintf(stderr, "isde-dm: D-Bus: acquired %s\n", DM_DBUS_SERVICE);
    dbus_error_free(&err);
    return 0;
}

void dm_dbus_cleanup(Dm *dm)
{
    if (dm->dbus) {
        dbus_connection_unref(dm->dbus);
        dm->dbus = NULL;
    }
}

void dm_dbus_dispatch(Dm *dm)
{
    if (!dm->dbus) {
        return;
    }
    /* Read incoming data from the bus, then dispatch queued messages */
    dbus_connection_read_write(dm->dbus, 0);
    while (dbus_connection_dispatch(dm->dbus) ==
           DBUS_DISPATCH_DATA_REMAINS) {
        /* drain */
    }
}

int dm_dbus_get_fd(Dm *dm)
{
    if (!dm->dbus) {
        return -1;
    }

    int fd = -1;
    dbus_connection_get_unix_fd(dm->dbus, &fd);
    return fd;
}

/* ---------- Signal emitters ---------- */

static void emit_signal(Dm *dm, const char *name)
{
    if (!dm->dbus) {
        return;
    }
    DBusMessage *sig = dbus_message_new_signal(DM_DBUS_PATH,
                                                DM_DBUS_INTERFACE,
                                                name);
    if (sig) {
        dbus_connection_send(dm->dbus, sig, NULL);
        dbus_message_unref(sig);
        dbus_connection_flush(dm->dbus);
    }
}

void dm_dbus_emit_session_started(Dm *dm, const char *username,
                                  const char *session)
{
    if (!dm->dbus) {
        return;
    }
    DBusMessage *sig = dbus_message_new_signal(DM_DBUS_PATH,
                                                DM_DBUS_INTERFACE,
                                                "SessionStarted");
    if (sig) {
        dbus_message_append_args(sig,
                                 DBUS_TYPE_STRING, &username,
                                 DBUS_TYPE_STRING, &session,
                                 DBUS_TYPE_INVALID);
        dbus_connection_send(dm->dbus, sig, NULL);
        dbus_message_unref(sig);
        dbus_connection_flush(dm->dbus);
    }
}

void dm_dbus_emit_session_ended(Dm *dm, const char *username)
{
    if (!dm->dbus) {
        return;
    }
    DBusMessage *sig = dbus_message_new_signal(DM_DBUS_PATH,
                                                DM_DBUS_INTERFACE,
                                                "SessionEnded");
    if (sig) {
        dbus_message_append_args(sig,
                                 DBUS_TYPE_STRING, &username,
                                 DBUS_TYPE_INVALID);
        dbus_connection_send(dm->dbus, sig, NULL);
        dbus_message_unref(sig);
        dbus_connection_flush(dm->dbus);
    }
}

void dm_dbus_emit_locked(Dm *dm)
{
    emit_signal(dm, "Locked");
}

void dm_dbus_emit_unlocked(Dm *dm)
{
    emit_signal(dm, "Unlocked");
}
