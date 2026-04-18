#define _POSIX_C_SOURCE 200809L
/*
 * dbus.c — D-Bus system bus interface for isde-mountd
 *
 * Bus name:  org.isde.DiskManager
 * Object:    /org/isde/DiskManager
 * Interface: org.isde.DiskManager
 *
 * Methods:  ListDevices, Mount, Unmount, Eject
 * Signals:  DeviceAdded, DeviceRemoved, DeviceMounted, DeviceUnmounted
 */
#include "mountd.h"

#include <stdio.h>
#include <string.h>
#include <dbus/dbus.h>

/* ---------- introspection XML ---------- */

static const char *introspect_xml =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object "
    "Introspection 1.0//EN\" \"http://www.freedesktop.org/standards/"
    "dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"" MOUNTD_DBUS_INTERFACE "\">\n"
    "    <method name=\"ListDevices\">\n"
    "      <arg name=\"devices\" type=\"a(sssssbb)\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Mount\">\n"
    "      <arg name=\"device_path\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"success\" type=\"b\" direction=\"out\"/>\n"
    "      <arg name=\"result\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Unmount\">\n"
    "      <arg name=\"device_path\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"success\" type=\"b\" direction=\"out\"/>\n"
    "      <arg name=\"result\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Eject\">\n"
    "      <arg name=\"device_path\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"success\" type=\"b\" direction=\"out\"/>\n"
    "      <arg name=\"result\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <signal name=\"DeviceAdded\">\n"
    "      <arg name=\"device_path\" type=\"s\"/>\n"
    "      <arg name=\"label\" type=\"s\"/>\n"
    "      <arg name=\"fs_type\" type=\"s\"/>\n"
    "    </signal>\n"
    "    <signal name=\"DeviceRemoved\">\n"
    "      <arg name=\"device_path\" type=\"s\"/>\n"
    "    </signal>\n"
    "    <signal name=\"DeviceMounted\">\n"
    "      <arg name=\"device_path\" type=\"s\"/>\n"
    "      <arg name=\"mount_point\" type=\"s\"/>\n"
    "    </signal>\n"
    "    <signal name=\"DeviceUnmounted\">\n"
    "      <arg name=\"device_path\" type=\"s\"/>\n"
    "    </signal>\n"
    "  </interface>\n"
    "</node>\n";

/* ---------- helper: get caller UID ---------- */

static dbus_bool_t get_caller_uid(DBusConnection *conn, DBusMessage *msg,
                                  unsigned long *uid)
{
    const char *sender = dbus_message_get_sender(msg);
    if (!sender) {
        return FALSE;
    }

    DBusError err;
    dbus_error_init(&err);
    *uid = dbus_bus_get_unix_user(conn, sender, &err);
    if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return FALSE;
    }
    return TRUE;
}

/* ---------- method: ListDevices ---------- */

static DBusMessage *handle_list_devices(MountDaemon *md, DBusMessage *msg)
{
    DBusMessage *reply = dbus_message_new_method_return(msg);
    DBusMessageIter iter, array;

    dbus_message_iter_init_append(reply, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(sssssbb)",
                                     &array);

    for (int i = 0; i < md->ndevices; i++) {
        Device *d = &md->devices[i];
        DBusMessageIter st;
        dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT, NULL, &st);

        const char *dev_path = d->dev_path;
        const char *label    = d->label;
        const char *vendor   = d->vendor;
        const char *fs_type  = d->fs_type;
        const char *mp       = d->mount_point;
        dbus_bool_t mounted  = d->is_mounted;
        dbus_bool_t eject    = d->is_ejectable;

        dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &dev_path);
        dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &label);
        dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &vendor);
        dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &fs_type);
        dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &mp);
        dbus_message_iter_append_basic(&st, DBUS_TYPE_BOOLEAN, &mounted);
        dbus_message_iter_append_basic(&st, DBUS_TYPE_BOOLEAN, &eject);

        dbus_message_iter_close_container(&array, &st);
    }

    dbus_message_iter_close_container(&iter, &array);
    return reply;
}

/* ---------- method: Mount ---------- */

static DBusMessage *handle_mount(MountDaemon *md, DBusConnection *conn,
                                 DBusMessage *msg)
{
    const char *dev_path = NULL;
    if (!dbus_message_get_args(msg, NULL,
                               DBUS_TYPE_STRING, &dev_path,
                               DBUS_TYPE_INVALID) || !dev_path) {
        return dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
                                      "Expected (device_path: s)");
    }

    unsigned long uid = 0;
    if (!get_caller_uid(conn, msg, &uid)) {
        return dbus_message_new_error(msg, DBUS_ERROR_AUTH_FAILED,
                                      "Cannot determine caller UID");
    }

    char mp[MOUNT_POINT_LEN];
    char errbuf[256];
    int ok = mountd_do_mount(md, dev_path, uid, mp, sizeof(mp),
                             errbuf, sizeof(errbuf));

    DBusMessage *reply = dbus_message_new_method_return(msg);
    dbus_bool_t success = (ok == 0);
    const char *result = success ? mp : errbuf;
    dbus_message_append_args(reply,
                             DBUS_TYPE_BOOLEAN, &success,
                             DBUS_TYPE_STRING, &result,
                             DBUS_TYPE_INVALID);

    if (success) {
        mountd_dbus_emit_device_mounted(md, dev_path, mp);
    }
    return reply;
}

/* ---------- method: Unmount ---------- */

static DBusMessage *handle_unmount(MountDaemon *md, DBusMessage *msg)
{
    const char *dev_path = NULL;
    if (!dbus_message_get_args(msg, NULL,
                               DBUS_TYPE_STRING, &dev_path,
                               DBUS_TYPE_INVALID) || !dev_path) {
        return dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
                                      "Expected (device_path: s)");
    }

    char errbuf[256];
    int ok = mountd_do_unmount(md, dev_path, errbuf, sizeof(errbuf));

    DBusMessage *reply = dbus_message_new_method_return(msg);
    dbus_bool_t success = (ok == 0);
    const char *result = success ? "" : errbuf;
    dbus_message_append_args(reply,
                             DBUS_TYPE_BOOLEAN, &success,
                             DBUS_TYPE_STRING, &result,
                             DBUS_TYPE_INVALID);

    if (success) {
        mountd_dbus_emit_device_unmounted(md, dev_path);
    }
    return reply;
}

/* ---------- method: Eject ---------- */

static DBusMessage *handle_eject(MountDaemon *md, DBusMessage *msg)
{
    const char *dev_path = NULL;
    if (!dbus_message_get_args(msg, NULL,
                               DBUS_TYPE_STRING, &dev_path,
                               DBUS_TYPE_INVALID) || !dev_path) {
        return dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
                                      "Expected (device_path: s)");
    }

    char errbuf[256];
    int ok = mountd_do_eject(md, dev_path, errbuf, sizeof(errbuf));

    DBusMessage *reply = dbus_message_new_method_return(msg);
    dbus_bool_t success = (ok == 0);
    const char *result = success ? "" : errbuf;
    dbus_message_append_args(reply,
                             DBUS_TYPE_BOOLEAN, &success,
                             DBUS_TYPE_STRING, &result,
                             DBUS_TYPE_INVALID);

    if (success) {
        mountd_dbus_emit_device_unmounted(md, dev_path);
        mountd_dbus_emit_device_removed(md, dev_path);
    }
    return reply;
}

/* ---------- method dispatch ---------- */

static DBusHandlerResult
handle_method(DBusConnection *conn, DBusMessage *msg, void *userdata)
{
    MountDaemon *md = (MountDaemon *)userdata;

    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const char *iface  = dbus_message_get_interface(msg);
    const char *method = dbus_message_get_member(msg);
    if (!method) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    /* Introspection */
    if (iface && strcmp(iface, "org.freedesktop.DBus.Introspectable") == 0 &&
        strcmp(method, "Introspect") == 0) {
        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_message_append_args(reply,
                                 DBUS_TYPE_STRING, &introspect_xml,
                                 DBUS_TYPE_INVALID);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* Only handle our interface */
    if (!iface || strcmp(iface, MOUNTD_DBUS_INTERFACE) != 0) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    DBusMessage *reply = NULL;

    fprintf(stderr, "isde-mountd: D-Bus method: %s\n", method);

    if (strcmp(method, "ListDevices") == 0) {
        reply = handle_list_devices(md, msg);
    } else if (strcmp(method, "Mount") == 0) {
        reply = handle_mount(md, conn, msg);
    } else if (strcmp(method, "Unmount") == 0) {
        reply = handle_unmount(md, msg);
    } else if (strcmp(method, "Eject") == 0) {
        reply = handle_eject(md, msg);
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

/* ---------- init / cleanup / dispatch ---------- */

int mountd_dbus_init(MountDaemon *md)
{
    DBusError err;
    dbus_error_init(&err);

    md->dbus = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!md->dbus) {
        fprintf(stderr, "isde-mountd: D-Bus: %s\n",
                err.message ? err.message : "cannot connect");
        dbus_error_free(&err);
        return -1;
    }

    dbus_connection_set_exit_on_disconnect(md->dbus, FALSE);

    int ret = dbus_bus_request_name(md->dbus, MOUNTD_DBUS_SERVICE,
                                    DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "isde-mountd: D-Bus: cannot acquire %s: %s\n",
                MOUNTD_DBUS_SERVICE,
                err.message ? err.message : "name already owned");
        dbus_error_free(&err);
        dbus_connection_unref(md->dbus);
        md->dbus = NULL;
        return -1;
    }

    dbus_connection_register_object_path(md->dbus, MOUNTD_DBUS_PATH,
                                         &vtable, md);

    fprintf(stderr, "isde-mountd: D-Bus: acquired %s\n", MOUNTD_DBUS_SERVICE);
    dbus_error_free(&err);
    return 0;
}

void mountd_dbus_cleanup(MountDaemon *md)
{
    if (md->dbus) {
        dbus_connection_unref(md->dbus);
        md->dbus = NULL;
    }
}

void mountd_dbus_dispatch(MountDaemon *md)
{
    if (!md->dbus) {
        return;
    }
    dbus_connection_read_write(md->dbus, 0);
    while (dbus_connection_dispatch(md->dbus) ==
           DBUS_DISPATCH_DATA_REMAINS) {
        /* drain */
    }
}

int mountd_dbus_get_fd(MountDaemon *md)
{
    if (!md->dbus) {
        return -1;
    }
    int fd = -1;
    dbus_connection_get_unix_fd(md->dbus, &fd);
    return fd;
}

/* ---------- signal emitters ---------- */

void mountd_dbus_emit_device_added(MountDaemon *md, const Device *dev)
{
    if (!md->dbus) {
        return;
    }
    DBusMessage *sig = dbus_message_new_signal(MOUNTD_DBUS_PATH,
                                                MOUNTD_DBUS_INTERFACE,
                                                "DeviceAdded");
    if (sig) {
        const char *dp = dev->dev_path;
        const char *lb = dev->label;
        const char *fs = dev->fs_type;
        dbus_message_append_args(sig,
                                 DBUS_TYPE_STRING, &dp,
                                 DBUS_TYPE_STRING, &lb,
                                 DBUS_TYPE_STRING, &fs,
                                 DBUS_TYPE_INVALID);
        dbus_connection_send(md->dbus, sig, NULL);
        dbus_message_unref(sig);
        dbus_connection_flush(md->dbus);
    }
}

void mountd_dbus_emit_device_removed(MountDaemon *md, const char *dev_path)
{
    if (!md->dbus) {
        return;
    }
    DBusMessage *sig = dbus_message_new_signal(MOUNTD_DBUS_PATH,
                                                MOUNTD_DBUS_INTERFACE,
                                                "DeviceRemoved");
    if (sig) {
        dbus_message_append_args(sig,
                                 DBUS_TYPE_STRING, &dev_path,
                                 DBUS_TYPE_INVALID);
        dbus_connection_send(md->dbus, sig, NULL);
        dbus_message_unref(sig);
        dbus_connection_flush(md->dbus);
    }
}

void mountd_dbus_emit_device_mounted(MountDaemon *md, const char *dev_path,
                                     const char *mount_point)
{
    if (!md->dbus) {
        return;
    }
    DBusMessage *sig = dbus_message_new_signal(MOUNTD_DBUS_PATH,
                                                MOUNTD_DBUS_INTERFACE,
                                                "DeviceMounted");
    if (sig) {
        dbus_message_append_args(sig,
                                 DBUS_TYPE_STRING, &dev_path,
                                 DBUS_TYPE_STRING, &mount_point,
                                 DBUS_TYPE_INVALID);
        dbus_connection_send(md->dbus, sig, NULL);
        dbus_message_unref(sig);
        dbus_connection_flush(md->dbus);
    }
}

void mountd_dbus_emit_device_unmounted(MountDaemon *md, const char *dev_path)
{
    if (!md->dbus) {
        return;
    }
    DBusMessage *sig = dbus_message_new_signal(MOUNTD_DBUS_PATH,
                                                MOUNTD_DBUS_INTERFACE,
                                                "DeviceUnmounted");
    if (sig) {
        dbus_message_append_args(sig,
                                 DBUS_TYPE_STRING, &dev_path,
                                 DBUS_TYPE_INVALID);
        dbus_connection_send(md->dbus, sig, NULL);
        dbus_message_unref(sig);
        dbus_connection_flush(md->dbus);
    }
}
