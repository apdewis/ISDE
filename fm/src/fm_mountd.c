#define _POSIX_C_SOURCE 200809L
/*
 * fm_mountd.c -- D-Bus client for isde-mountd
 *
 * Connects to the system bus to call Mount/Unmount/Eject on
 * org.isde.DiskManager and subscribes to device change signals
 * so the places sidebar stays current.
 */
#include "fm.h"
#include "fm_mountd.h"

#include <stdio.h>
#include <string.h>

/* ---------- signal filter ---------- */

static DBusHandlerResult
signal_filter(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    (void)conn;
    FmApp *app = (FmApp *)user_data;

    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_SIGNAL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *iface = dbus_message_get_interface(msg);
    if (!iface || strcmp(iface, FM_MOUNTD_INTERFACE) != 0)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *member = dbus_message_get_member(msg);
    if (!member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (strcmp(member, "DeviceAdded") == 0) {
        const char *dev_path = NULL, *label = NULL, *fs_type = NULL;
        if (dbus_message_get_args(msg, NULL,
                                   DBUS_TYPE_STRING, &dev_path,
                                   DBUS_TYPE_STRING, &label,
                                   DBUS_TYPE_STRING, &fs_type,
                                   DBUS_TYPE_INVALID)) {
            if (app->mountd_ndevices < FM_MAX_DEVICES) {
                FmDeviceInfo *d = &app->mountd_devices[app->mountd_ndevices++];
                memset(d, 0, sizeof(*d));
                snprintf(d->dev_path, sizeof(d->dev_path), "%s", dev_path);
                snprintf(d->label, sizeof(d->label), "%s", label);
                snprintf(d->fs_type, sizeof(d->fs_type), "%s", fs_type);
            }
            for (int i = 0; i < app->nwindows; i++)
                places_refresh_devices(app->windows[i]);
            fprintf(stderr, "isde-fm: mountd: device added: %s (%s)\n",
                    dev_path, label);
        }
    } else if (strcmp(member, "DeviceRemoved") == 0) {
        const char *dev_path = NULL;
        if (dbus_message_get_args(msg, NULL,
                                   DBUS_TYPE_STRING, &dev_path,
                                   DBUS_TYPE_INVALID)) {
            /* Save mount point before removing, for navigate-away check */
            char old_mount[FM_MOUNT_POINT_LEN] = {0};
            for (int i = 0; i < app->mountd_ndevices; i++) {
                if (strcmp(app->mountd_devices[i].dev_path, dev_path) == 0) {
                    if (app->mountd_devices[i].is_mounted)
                        snprintf(old_mount, sizeof(old_mount), "%s",
                                 app->mountd_devices[i].mount_point);
                    app->mountd_ndevices--;
                    if (i < app->mountd_ndevices)
                        app->mountd_devices[i] = app->mountd_devices[app->mountd_ndevices];
                    break;
                }
            }
            for (int i = 0; i < app->nwindows; i++) {
                Fm *fm = app->windows[i];
                places_refresh_devices(fm);
                /* Navigate away if viewing the removed mount */
                if (old_mount[0] &&
                    strncmp(fm->cwd, old_mount, strlen(old_mount)) == 0) {
                    const char *home = getenv("HOME");
                    fm_navigate(fm, home ? home : "/");
                }
            }
            fprintf(stderr, "isde-fm: mountd: device removed: %s\n", dev_path);
        }
    } else if (strcmp(member, "DeviceMounted") == 0) {
        const char *dev_path = NULL, *mount_point = NULL;
        if (dbus_message_get_args(msg, NULL,
                                   DBUS_TYPE_STRING, &dev_path,
                                   DBUS_TYPE_STRING, &mount_point,
                                   DBUS_TYPE_INVALID)) {
            for (int i = 0; i < app->mountd_ndevices; i++) {
                if (strcmp(app->mountd_devices[i].dev_path, dev_path) == 0) {
                    app->mountd_devices[i].is_mounted = 1;
                    snprintf(app->mountd_devices[i].mount_point,
                             sizeof(app->mountd_devices[i].mount_point),
                             "%s", mount_point);
                    break;
                }
            }
            for (int i = 0; i < app->nwindows; i++)
                places_refresh_devices(app->windows[i]);
        }
    } else if (strcmp(member, "DeviceUnmounted") == 0) {
        const char *dev_path = NULL;
        if (dbus_message_get_args(msg, NULL,
                                   DBUS_TYPE_STRING, &dev_path,
                                   DBUS_TYPE_INVALID)) {
            char old_mount[FM_MOUNT_POINT_LEN] = {0};
            for (int i = 0; i < app->mountd_ndevices; i++) {
                if (strcmp(app->mountd_devices[i].dev_path, dev_path) == 0) {
                    snprintf(old_mount, sizeof(old_mount), "%s",
                             app->mountd_devices[i].mount_point);
                    app->mountd_devices[i].is_mounted = 0;
                    app->mountd_devices[i].mount_point[0] = '\0';
                    break;
                }
            }
            for (int i = 0; i < app->nwindows; i++) {
                Fm *fm = app->windows[i];
                places_refresh_devices(fm);
                if (old_mount[0] &&
                    strncmp(fm->cwd, old_mount, strlen(old_mount)) == 0) {
                    const char *home = getenv("HOME");
                    fm_navigate(fm, home ? home : "/");
                }
            }
        }
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}

/* ---------- D-Bus fd dispatch callback ---------- */

static void mountd_dbus_cb(IswPointer client_data, int *fd, IswInputId *id)
{
    (void)fd; (void)id;
    FmApp *app = (FmApp *)client_data;
    if (app->mountd_bus) {
        dbus_connection_read_write(app->mountd_bus, 0);
        while (dbus_connection_dispatch(app->mountd_bus) ==
               DBUS_DISPATCH_DATA_REMAINS) {
            /* drain all queued messages */
        }
    }
}

/* ---------- init / cleanup ---------- */

int fm_mountd_init(FmApp *app)
{
    DBusError err;
    dbus_error_init(&err);

    app->mountd_bus = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!app->mountd_bus) {
        fprintf(stderr, "isde-fm: mountd: cannot connect to system bus: %s\n",
                err.message ? err.message : "(unknown)");
        dbus_error_free(&err);
        return -1;
    }

    dbus_connection_set_exit_on_disconnect(app->mountd_bus, FALSE);

    /* Probe mountd by calling ListDevices; if it fails, mountd isn't running */
    DBusMessage *probe = dbus_message_new_method_call(
        FM_MOUNTD_SERVICE, FM_MOUNTD_PATH,
        FM_MOUNTD_INTERFACE, "ListDevices");
    if (!probe) {
        dbus_connection_unref(app->mountd_bus);
        app->mountd_bus = NULL;
        dbus_error_free(&err);
        return -1;
    }

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        app->mountd_bus, probe, 2000, &err);
    dbus_message_unref(probe);

    if (!reply) {
        fprintf(stderr, "isde-fm: mountd not available: %s\n",
                dbus_error_is_set(&err) ? err.message : "no reply");
        dbus_error_free(&err);
        dbus_connection_unref(app->mountd_bus);
        app->mountd_bus = NULL;
        return -1;
    }

    /* Parse initial device list */
    app->mountd_ndevices = 0;
    DBusMessageIter iter, array;
    if (dbus_message_iter_init(reply, &iter) &&
        dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {

        dbus_message_iter_recurse(&iter, &array);
        while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRUCT) {
            if (app->mountd_ndevices >= FM_MAX_DEVICES)
                break;

            DBusMessageIter st;
            dbus_message_iter_recurse(&array, &st);
            FmDeviceInfo *d = &app->mountd_devices[app->mountd_ndevices];
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

            app->mountd_ndevices++;
            dbus_message_iter_next(&array);
        }
    }
    dbus_message_unref(reply);

    /* Subscribe to signals */
    dbus_bus_add_match(app->mountd_bus,
        "type='signal',interface='" FM_MOUNTD_INTERFACE "',"
        "member='DeviceAdded'", NULL);
    dbus_bus_add_match(app->mountd_bus,
        "type='signal',interface='" FM_MOUNTD_INTERFACE "',"
        "member='DeviceRemoved'", NULL);
    dbus_bus_add_match(app->mountd_bus,
        "type='signal',interface='" FM_MOUNTD_INTERFACE "',"
        "member='DeviceMounted'", NULL);
    dbus_bus_add_match(app->mountd_bus,
        "type='signal',interface='" FM_MOUNTD_INTERFACE "',"
        "member='DeviceUnmounted'", NULL);

    dbus_connection_add_filter(app->mountd_bus, signal_filter, app, NULL);

    /* Integrate with Xt event loop via the D-Bus fd */
    int fd = -1;
    dbus_connection_get_unix_fd(app->mountd_bus, &fd);
    if (fd >= 0) {
        app->mountd_input_id = IswAppAddInput(app->app, fd,
                                               (IswPointer)IswInputReadMask,
                                               mountd_dbus_cb, app);
    }

    app->has_mountd = 1;
    fprintf(stderr, "isde-fm: mountd connected, %d devices\n",
            app->mountd_ndevices);
    dbus_error_free(&err);
    return 0;
}

void fm_mountd_cleanup(FmApp *app)
{
    if (app->mountd_bus) {
        if (app->mountd_input_id) {
            IswRemoveInput(app->mountd_input_id);
            app->mountd_input_id = 0;
        }
        dbus_connection_remove_filter(app->mountd_bus, signal_filter, app);
        dbus_connection_unref(app->mountd_bus);
        app->mountd_bus = NULL;
    }
    app->has_mountd = 0;
    app->mountd_ndevices = 0;
}

/* ---------- method calls ---------- */

static int call_method(FmApp *app, const char *method,
                       const char *dev_path,
                       char *result, size_t result_len)
{
    if (!app->mountd_bus) {
        snprintf(result, result_len, "mountd not connected");
        return -1;
    }

    DBusMessage *msg = dbus_message_new_method_call(
        FM_MOUNTD_SERVICE, FM_MOUNTD_PATH,
        FM_MOUNTD_INTERFACE, method);
    if (!msg) {
        snprintf(result, result_len, "cannot create D-Bus message");
        return -1;
    }

    dbus_message_append_args(msg,
                             DBUS_TYPE_STRING, &dev_path,
                             DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        app->mountd_bus, msg, 5000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        if (dbus_error_is_set(&err))
            snprintf(result, result_len, "%s", err.message);
        else
            snprintf(result, result_len, "no reply");
        dbus_error_free(&err);
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

int fm_mountd_mount(FmApp *app, const char *dev_path,
                    char *result, size_t result_len)
{
    return call_method(app, "Mount", dev_path, result, result_len);
}

int fm_mountd_unmount(FmApp *app, const char *dev_path,
                      char *result, size_t result_len)
{
    return call_method(app, "Unmount", dev_path, result, result_len);
}

int fm_mountd_eject(FmApp *app, const char *dev_path,
                    char *result, size_t result_len)
{
    return call_method(app, "Eject", dev_path, result, result_len);
}

/* ---------- lookup ---------- */

FmDeviceInfo *fm_mountd_find_by_label(FmApp *app, const char *label)
{
    for (int i = 0; i < app->mountd_ndevices; i++) {
        if (strcmp(app->mountd_devices[i].label, label) == 0)
            return &app->mountd_devices[i];
    }
    return NULL;
}

FmDeviceInfo *fm_mountd_find_by_mount_point(FmApp *app, const char *path)
{
    for (int i = 0; i < app->mountd_ndevices; i++) {
        if (app->mountd_devices[i].mount_point[0] &&
            strcmp(app->mountd_devices[i].mount_point, path) == 0)
            return &app->mountd_devices[i];
    }
    return NULL;
}
