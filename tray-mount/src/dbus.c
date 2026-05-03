#define _POSIX_C_SOURCE 200809L
/*
 * dbus.c — D-Bus client for isde-tray-mount
 *
 * Connects to the system bus to communicate with isde-mountd
 * (org.isde.DiskManager).  Subscribes to device change signals
 * and provides methods to call Mount/Unmount/Eject/ListDevices.
 */
#include "tray-mount.h"

#include <stdio.h>
#include <string.h>

/* ---------- signal filter ---------- */

static DBusHandlerResult
signal_filter(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    (void)conn;
    TrayMount *tm = (TrayMount *)user_data;

    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_SIGNAL) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const char *iface = dbus_message_get_interface(msg);
    if (!iface || strcmp(iface, MOUNTD_DBUS_INTERFACE) != 0) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const char *member = dbus_message_get_member(msg);
    if (!member) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (strcmp(member, "DeviceAdded") == 0) {
        const char *dev_path = NULL, *label = NULL, *fs_type = NULL;
        if (dbus_message_get_args(msg, NULL,
                                   DBUS_TYPE_STRING, &dev_path,
                                   DBUS_TYPE_STRING, &label,
                                   DBUS_TYPE_STRING, &fs_type,
                                   DBUS_TYPE_INVALID)) {
            if (tm->ndevices < MAX_DEVICES) {
                DeviceInfo *d = &tm->devices[tm->ndevices++];
                memset(d, 0, sizeof(*d));
                snprintf(d->dev_path, sizeof(d->dev_path), "%s", dev_path);
                snprintf(d->label, sizeof(d->label), "%s", label);
                snprintf(d->fs_type, sizeof(d->fs_type), "%s", fs_type);
            }
            fprintf(stderr, "isde-tray-mount: device added: %s\n", dev_path);
        }
    } else if (strcmp(member, "DeviceRemoved") == 0) {
        const char *dev_path = NULL;
        if (dbus_message_get_args(msg, NULL,
                                   DBUS_TYPE_STRING, &dev_path,
                                   DBUS_TYPE_INVALID)) {
            for (int i = 0; i < tm->ndevices; i++) {
                if (strcmp(tm->devices[i].dev_path, dev_path) == 0) {
                    tm->ndevices--;
                    if (i < tm->ndevices) {
                        tm->devices[i] = tm->devices[tm->ndevices];
                    }
                    break;
                }
            }
            fprintf(stderr, "isde-tray-mount: device removed: %s\n",
                    dev_path);
        }
    } else if (strcmp(member, "DeviceMounted") == 0) {
        const char *dev_path = NULL, *mount_point = NULL;
        if (dbus_message_get_args(msg, NULL,
                                   DBUS_TYPE_STRING, &dev_path,
                                   DBUS_TYPE_STRING, &mount_point,
                                   DBUS_TYPE_INVALID)) {
            for (int i = 0; i < tm->ndevices; i++) {
                if (strcmp(tm->devices[i].dev_path, dev_path) == 0) {
                    tm->devices[i].is_mounted = 1;
                    snprintf(tm->devices[i].mount_point,
                             sizeof(tm->devices[i].mount_point),
                             "%s", mount_point);
                    break;
                }
            }
        }
    } else if (strcmp(member, "DeviceUnmounted") == 0) {
        const char *dev_path = NULL;
        if (dbus_message_get_args(msg, NULL,
                                   DBUS_TYPE_STRING, &dev_path,
                                   DBUS_TYPE_INVALID)) {
            for (int i = 0; i < tm->ndevices; i++) {
                if (strcmp(tm->devices[i].dev_path, dev_path) == 0) {
                    tm->devices[i].is_mounted = 0;
                    tm->devices[i].mount_point[0] = '\0';
                    break;
                }
            }
        }
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}

/* ---------- init / cleanup ---------- */

int tm_dbus_init(TrayMount *tm)
{
    DBusError err;
    dbus_error_init(&err);

    tm->system_bus = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!tm->system_bus) {
        fprintf(stderr, "isde-tray-mount: D-Bus system bus: %s\n",
                err.message ? err.message : "cannot connect");
        dbus_error_free(&err);
        return -1;
    }

    dbus_connection_set_exit_on_disconnect(tm->system_bus, FALSE);

    /* Subscribe to DiskManager signals */
    dbus_bus_add_match(tm->system_bus,
        "type='signal',"
        "interface='" MOUNTD_DBUS_INTERFACE "',"
        "member='DeviceAdded'",
        &err);
    dbus_bus_add_match(tm->system_bus,
        "type='signal',"
        "interface='" MOUNTD_DBUS_INTERFACE "',"
        "member='DeviceRemoved'",
        NULL);
    dbus_bus_add_match(tm->system_bus,
        "type='signal',"
        "interface='" MOUNTD_DBUS_INTERFACE "',"
        "member='DeviceMounted'",
        NULL);
    dbus_bus_add_match(tm->system_bus,
        "type='signal',"
        "interface='" MOUNTD_DBUS_INTERFACE "',"
        "member='DeviceUnmounted'",
        NULL);

    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "isde-tray-mount: D-Bus match: %s\n", err.message);
        dbus_error_free(&err);
    }

    dbus_connection_add_filter(tm->system_bus, signal_filter, tm, NULL);

    fprintf(stderr, "isde-tray-mount: connected to system bus\n");
    dbus_error_free(&err);
    return 0;
}

void tm_dbus_cleanup(TrayMount *tm)
{
    if (tm->system_bus) {
        dbus_connection_remove_filter(tm->system_bus, signal_filter, tm);
        dbus_connection_unref(tm->system_bus);
        tm->system_bus = NULL;
    }
}

/* ---------- ListDevices ---------- */

int tm_dbus_list_devices(TrayMount *tm)
{
    if (!tm->system_bus) {
        return -1;
    }

    DBusMessage *msg = dbus_message_new_method_call(
        MOUNTD_DBUS_SERVICE, MOUNTD_DBUS_PATH,
        MOUNTD_DBUS_INTERFACE, "ListDevices");
    if (!msg) {
        return -1;
    }

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        tm->system_bus, msg, 2000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        if (dbus_error_is_set(&err)) {
            fprintf(stderr, "isde-tray-mount: ListDevices: %s\n",
                    err.message);
            dbus_error_free(&err);
        }
        return -1;
    }

    /* Parse the array of structs */
    tm->ndevices = 0;

    DBusMessageIter iter, array;
    if (!dbus_message_iter_init(reply, &iter)) {
        dbus_message_unref(reply);
        return -1;
    }

    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return -1;
    }

    dbus_message_iter_recurse(&iter, &array);

    while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRUCT) {
        if (tm->ndevices >= MAX_DEVICES) {
            break;
        }

        DBusMessageIter st;
        dbus_message_iter_recurse(&array, &st);

        DeviceInfo *d = &tm->devices[tm->ndevices];
        memset(d, 0, sizeof(*d));

        const char *s;
        dbus_bool_t b;

        dbus_message_iter_get_basic(&st, &s);
        snprintf(d->dev_path, sizeof(d->dev_path), "%s", s);
        dbus_message_iter_next(&st);

        dbus_message_iter_get_basic(&st, &s);
        snprintf(d->label, sizeof(d->label), "%s", s);
        dbus_message_iter_next(&st);

        dbus_message_iter_get_basic(&st, &s);
        snprintf(d->vendor, sizeof(d->vendor), "%s", s);
        dbus_message_iter_next(&st);

        dbus_message_iter_get_basic(&st, &s);
        snprintf(d->fs_type, sizeof(d->fs_type), "%s", s);
        dbus_message_iter_next(&st);

        dbus_message_iter_get_basic(&st, &s);
        snprintf(d->mount_point, sizeof(d->mount_point), "%s", s);
        dbus_message_iter_next(&st);

        dbus_message_iter_get_basic(&st, &b);
        d->is_mounted = b;
        dbus_message_iter_next(&st);

        dbus_message_iter_get_basic(&st, &b);
        d->is_ejectable = b;

        if (dbus_message_iter_next(&st) &&
            dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_BOOLEAN) {
            dbus_message_iter_get_basic(&st, &b);
            d->is_luks = b;
        }

        tm->ndevices++;
        dbus_message_iter_next(&array);
    }

    dbus_message_unref(reply);
    return 0;
}

/* ---------- Mount / Unmount / Eject ---------- */

static int call_device_method(TrayMount *tm, const char *method,
                              const char *dev_path,
                              char *result, size_t result_len)
{
    if (!tm->system_bus) {
        snprintf(result, result_len, "D-Bus not connected");
        return -1;
    }

    DBusMessage *msg = dbus_message_new_method_call(
        MOUNTD_DBUS_SERVICE, MOUNTD_DBUS_PATH,
        MOUNTD_DBUS_INTERFACE, method);
    if (!msg) {
        snprintf(result, result_len, "Cannot create D-Bus message");
        return -1;
    }

    dbus_message_append_args(msg,
                             DBUS_TYPE_STRING, &dev_path,
                             DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        tm->system_bus, msg, 5000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        if (dbus_error_is_set(&err)) {
            snprintf(result, result_len, "%s", err.message);
            dbus_error_free(&err);
        } else {
            snprintf(result, result_len, "No reply");
        }
        return -1;
    }

    dbus_bool_t success = FALSE;
    const char *res_str = "";
    dbus_message_get_args(reply, NULL,
                          DBUS_TYPE_BOOLEAN, &success,
                          DBUS_TYPE_STRING, &res_str,
                          DBUS_TYPE_INVALID);

    snprintf(result, result_len, "%s", res_str);
    dbus_message_unref(reply);
    dbus_error_free(&err);
    return success ? 0 : -1;
}

int tm_dbus_mount(TrayMount *tm, const char *dev_path,
                  const char *passphrase,
                  char *result, size_t result_len)
{
    if (!tm->system_bus) {
        snprintf(result, result_len, "D-Bus not connected");
        return -1;
    }

    DBusMessage *msg = dbus_message_new_method_call(
        MOUNTD_DBUS_SERVICE, MOUNTD_DBUS_PATH,
        MOUNTD_DBUS_INTERFACE, "Mount");
    if (!msg) {
        snprintf(result, result_len, "Cannot create D-Bus message");
        return -1;
    }

    const char *pw = passphrase ? passphrase : "";
    dbus_message_append_args(msg,
                             DBUS_TYPE_STRING, &dev_path,
                             DBUS_TYPE_STRING, &pw,
                             DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        tm->system_bus, msg, 10000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        if (dbus_error_is_set(&err)) {
            snprintf(result, result_len, "%s", err.message);
            dbus_error_free(&err);
        } else {
            snprintf(result, result_len, "No reply");
        }
        return -1;
    }

    dbus_bool_t success = FALSE;
    const char *res_str = "";
    dbus_message_get_args(reply, NULL,
                          DBUS_TYPE_BOOLEAN, &success,
                          DBUS_TYPE_STRING, &res_str,
                          DBUS_TYPE_INVALID);

    snprintf(result, result_len, "%s", res_str);
    dbus_message_unref(reply);
    dbus_error_free(&err);
    return success ? 0 : -1;
}

int tm_dbus_unmount(TrayMount *tm, const char *dev_path,
                    char *errbuf, size_t errlen)
{
    return call_device_method(tm, "Unmount", dev_path, errbuf, errlen);
}

int tm_dbus_eject(TrayMount *tm, const char *dev_path,
                  char *errbuf, size_t errlen)
{
    return call_device_method(tm, "Eject", dev_path, errbuf, errlen);
}
