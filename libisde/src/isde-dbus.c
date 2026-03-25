#define _POSIX_C_SOURCE 200809L
/*
 * isde-dbus.c — D-Bus settings notification via libdbus-1
 */
#include "isde/isde-dbus.h"

#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SUBSCRIBERS 16

typedef struct {
    IsdeSettingsChangedCb cb;
    void *user_data;
} Subscriber;

struct IsdeDBus {
    DBusConnection *conn;
    int             fd;
    Subscriber      subs[MAX_SUBSCRIBERS];
    int             nsubs;
};

/* ---------- D-Bus message filter ---------- */

static DBusHandlerResult
filter_func(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    (void)conn;
    IsdeDBus *bus = (IsdeDBus *)user_data;

    if (dbus_message_is_signal(msg, ISDE_DBUS_INTERFACE, ISDE_DBUS_SIGNAL)) {
        const char *section = NULL;
        const char *key = NULL;
        DBusError err;
        dbus_error_init(&err);

        if (dbus_message_get_args(msg, &err,
                                  DBUS_TYPE_STRING, &section,
                                  DBUS_TYPE_STRING, &key,
                                  DBUS_TYPE_INVALID)) {
            for (int i = 0; i < bus->nsubs; i++)
                bus->subs[i].cb(section, key, bus->subs[i].user_data);
        }
        dbus_error_free(&err);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ---------- public API ---------- */

IsdeDBus *isde_dbus_init(void)
{
    DBusError err;
    dbus_error_init(&err);

    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "isde-dbus: connection failed: %s\n", err.message);
        dbus_error_free(&err);
        return NULL;
    }
    if (!conn)
        return NULL;

    /* Don't exit the process if D-Bus disconnects */
    dbus_connection_set_exit_on_disconnect(conn, FALSE);

    /* Subscribe to SettingsChanged signals */
    dbus_bus_add_match(conn,
        "type='signal',"
        "interface='" ISDE_DBUS_INTERFACE "',"
        "member='" ISDE_DBUS_SIGNAL "'",
        &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "isde-dbus: add_match failed: %s\n", err.message);
        dbus_error_free(&err);
    }

    IsdeDBus *bus = calloc(1, sizeof(*bus));
    bus->conn = conn;
    bus->fd = -1;

    /* Extract the underlying fd for event loop integration */
    int fd = -1;
    if (dbus_connection_get_unix_fd(conn, &fd))
        bus->fd = fd;

    /* Install message filter */
    dbus_connection_add_filter(conn, filter_func, bus, NULL);

    /* Flush any pending outgoing messages */
    dbus_connection_flush(conn);

    return bus;
}

void isde_dbus_free(IsdeDBus *bus)
{
    if (!bus) return;
    if (bus->conn) {
        dbus_connection_remove_filter(bus->conn, filter_func, bus);
        dbus_connection_unref(bus->conn);
    }
    free(bus);
}

int isde_dbus_get_fd(IsdeDBus *bus)
{
    return bus ? bus->fd : -1;
}

void isde_dbus_dispatch(IsdeDBus *bus)
{
    if (!bus || !bus->conn) return;

    /* Read incoming data and dispatch messages */
    dbus_connection_read_write(bus->conn, 0);
    while (dbus_connection_dispatch(bus->conn) == DBUS_DISPATCH_DATA_REMAINS)
        ;
}

void isde_dbus_settings_notify(IsdeDBus *bus,
                                const char *section,
                                const char *key)
{
    if (!bus || !bus->conn) return;

    DBusMessage *msg = dbus_message_new_signal(
        ISDE_DBUS_PATH,
        ISDE_DBUS_INTERFACE,
        ISDE_DBUS_SIGNAL);
    if (!msg) return;

    dbus_message_append_args(msg,
                             DBUS_TYPE_STRING, &section,
                             DBUS_TYPE_STRING, &key,
                             DBUS_TYPE_INVALID);

    dbus_connection_send(bus->conn, msg, NULL);
    dbus_connection_flush(bus->conn);
    dbus_message_unref(msg);
}

void isde_dbus_settings_subscribe(IsdeDBus *bus,
                                   IsdeSettingsChangedCb cb,
                                   void *user_data)
{
    if (!bus || bus->nsubs >= MAX_SUBSCRIBERS) return;
    bus->subs[bus->nsubs].cb = cb;
    bus->subs[bus->nsubs].user_data = user_data;
    bus->nsubs++;
}
