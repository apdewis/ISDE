#define _POSIX_C_SOURCE 200809L
/*
 * bluez.c — D-Bus client for BlueZ
 *
 * Queries adapters and devices via ObjectManager on the system bus.
 * Subscribes to InterfacesAdded/Removed and PropertiesChanged for
 * live state updates.
 */
#include "tray-bt.h"

#include <stdio.h>
#include <string.h>

/* ---------- helpers: extract properties from variant iterators ---------- */

static void parse_adapter_properties(AdapterInfo *a, DBusMessageIter *dict)
{
    while (dbus_message_iter_get_arg_type(dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, variant;
        dbus_message_iter_recurse(dict, &entry);

        const char *key;
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);
        dbus_message_iter_recurse(&entry, &variant);

        int vtype = dbus_message_iter_get_arg_type(&variant);

        if (strcmp(key, "Name") == 0 && vtype == DBUS_TYPE_STRING) {
            const char *v;
            dbus_message_iter_get_basic(&variant, &v);
            snprintf(a->name, sizeof(a->name), "%s", v);
        } else if (strcmp(key, "Address") == 0 && vtype == DBUS_TYPE_STRING) {
            const char *v;
            dbus_message_iter_get_basic(&variant, &v);
            snprintf(a->address, sizeof(a->address), "%s", v);
        } else if (strcmp(key, "Powered") == 0 && vtype == DBUS_TYPE_BOOLEAN) {
            dbus_bool_t v;
            dbus_message_iter_get_basic(&variant, &v);
            a->powered = v;
        } else if (strcmp(key, "Discovering") == 0 &&
                   vtype == DBUS_TYPE_BOOLEAN) {
            dbus_bool_t v;
            dbus_message_iter_get_basic(&variant, &v);
            a->discovering = v;
        }

        dbus_message_iter_next(dict);
    }
}

static void parse_device_properties(DeviceInfo *d, DBusMessageIter *dict)
{
    while (dbus_message_iter_get_arg_type(dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, variant;
        dbus_message_iter_recurse(dict, &entry);

        const char *key;
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);
        dbus_message_iter_recurse(&entry, &variant);

        int vtype = dbus_message_iter_get_arg_type(&variant);

        if (strcmp(key, "Name") == 0 && vtype == DBUS_TYPE_STRING) {
            const char *v;
            dbus_message_iter_get_basic(&variant, &v);
            snprintf(d->name, sizeof(d->name), "%s", v);
        } else if (strcmp(key, "Alias") == 0 && vtype == DBUS_TYPE_STRING) {
            if (d->name[0] == '\0') {
                const char *v;
                dbus_message_iter_get_basic(&variant, &v);
                snprintf(d->name, sizeof(d->name), "%s", v);
            }
        } else if (strcmp(key, "Address") == 0 && vtype == DBUS_TYPE_STRING) {
            const char *v;
            dbus_message_iter_get_basic(&variant, &v);
            snprintf(d->address, sizeof(d->address), "%s", v);
        } else if (strcmp(key, "Icon") == 0 && vtype == DBUS_TYPE_STRING) {
            const char *v;
            dbus_message_iter_get_basic(&variant, &v);
            snprintf(d->icon, sizeof(d->icon), "%s", v);
        } else if (strcmp(key, "Adapter") == 0 &&
                   vtype == DBUS_TYPE_OBJECT_PATH) {
            const char *v;
            dbus_message_iter_get_basic(&variant, &v);
            snprintf(d->adapter, sizeof(d->adapter), "%s", v);
        } else if (strcmp(key, "Paired") == 0 && vtype == DBUS_TYPE_BOOLEAN) {
            dbus_bool_t v;
            dbus_message_iter_get_basic(&variant, &v);
            d->paired = v;
        } else if (strcmp(key, "Connected") == 0 &&
                   vtype == DBUS_TYPE_BOOLEAN) {
            dbus_bool_t v;
            dbus_message_iter_get_basic(&variant, &v);
            d->connected = v;
        } else if (strcmp(key, "Trusted") == 0 && vtype == DBUS_TYPE_BOOLEAN) {
            dbus_bool_t v;
            dbus_message_iter_get_basic(&variant, &v);
            d->trusted = v;
        } else if (strcmp(key, "RSSI") == 0 && vtype == DBUS_TYPE_INT16) {
            int16_t v;
            dbus_message_iter_get_basic(&variant, &v);
            d->rssi = v;
        }

        dbus_message_iter_next(dict);
    }
}

/* ---------- signal filter ---------- */

static DeviceInfo *find_device_by_path(TrayBt *tb, const char *path)
{
    for (int i = 0; i < tb->ndevices; i++) {
        if (strcmp(tb->devices[i].path, path) == 0)
            return &tb->devices[i];
    }
    return NULL;
}

static int path_is_device(const char *path)
{
    /* BlueZ device paths: /org/bluez/hciX/dev_XX_XX_XX_XX_XX_XX */
    return strstr(path, "/dev_") != NULL;
}

static void handle_properties_changed(TrayBt *tb, DBusMessage *msg)
{
    const char *path = dbus_message_get_path(msg);
    if (!path)
        return;

    DBusMessageIter iter;
    if (!dbus_message_iter_init(msg, &iter))
        return;

    const char *iface;
    dbus_message_iter_get_basic(&iter, &iface);
    dbus_message_iter_next(&iter);

    if (strcmp(iface, BLUEZ_ADAPTER_IFACE) == 0 &&
        tb->has_adapter && strcmp(path, tb->adapter.path) == 0) {
        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
            DBusMessageIter dict;
            dbus_message_iter_recurse(&iter, &dict);
            parse_adapter_properties(&tb->adapter, &dict);
        }
        tray_bt_update_icon(tb);
        tb_menu_rebuild(tb);
        return;
    }

    if (strcmp(iface, BLUEZ_DEVICE_IFACE) == 0) {
        DeviceInfo *dev = find_device_by_path(tb, path);
        if (dev && dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
            DBusMessageIter dict;
            dbus_message_iter_recurse(&iter, &dict);
            parse_device_properties(dev, &dict);
            tray_bt_update_icon(tb);
            tb_menu_rebuild(tb);
        }
        return;
    }
}

static void handle_interfaces_added(TrayBt *tb, DBusMessage *msg)
{
    DBusMessageIter iter;
    if (!dbus_message_iter_init(msg, &iter))
        return;

    const char *path;
    dbus_message_iter_get_basic(&iter, &path);
    dbus_message_iter_next(&iter);

    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
        return;

    DBusMessageIter ifaces;
    dbus_message_iter_recurse(&iter, &ifaces);

    while (dbus_message_iter_get_arg_type(&ifaces) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&ifaces, &entry);

        const char *iface;
        dbus_message_iter_get_basic(&entry, &iface);
        dbus_message_iter_next(&entry);

        if (strcmp(iface, BLUEZ_DEVICE_IFACE) == 0 && path_is_device(path)) {
            if (tb->ndevices < MAX_DEVICES &&
                !find_device_by_path(tb, path)) {
                DeviceInfo *d = &tb->devices[tb->ndevices];
                memset(d, 0, sizeof(*d));
                snprintf(d->path, sizeof(d->path), "%s", path);

                DBusMessageIter props;
                dbus_message_iter_recurse(&entry, &props);
                parse_device_properties(d, &props);
                tb->ndevices++;
                tb_menu_rebuild(tb);
            }
        } else if (strcmp(iface, BLUEZ_ADAPTER_IFACE) == 0 &&
                   !tb->has_adapter) {
            memset(&tb->adapter, 0, sizeof(tb->adapter));
            snprintf(tb->adapter.path, sizeof(tb->adapter.path), "%s", path);

            DBusMessageIter props;
            dbus_message_iter_recurse(&entry, &props);
            parse_adapter_properties(&tb->adapter, &props);
            tb->has_adapter = 1;
            tray_bt_update_icon(tb);
            tb_menu_rebuild(tb);
        }

        dbus_message_iter_next(&ifaces);
    }
}

static void handle_interfaces_removed(TrayBt *tb, DBusMessage *msg)
{
    DBusMessageIter iter;
    if (!dbus_message_iter_init(msg, &iter))
        return;

    const char *path;
    dbus_message_iter_get_basic(&iter, &path);

    if (tb->has_adapter && strcmp(path, tb->adapter.path) == 0) {
        tb->has_adapter = 0;
        memset(&tb->adapter, 0, sizeof(tb->adapter));
        tb->ndevices = 0;
        tray_bt_update_icon(tb);
        tb_menu_rebuild(tb);
        return;
    }

    for (int i = 0; i < tb->ndevices; i++) {
        if (strcmp(tb->devices[i].path, path) == 0) {
            tb->ndevices--;
            if (i < tb->ndevices)
                tb->devices[i] = tb->devices[tb->ndevices];
            tb_menu_rebuild(tb);
            return;
        }
    }
}

static DBusHandlerResult
signal_filter(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    (void)conn;
    TrayBt *tb = (TrayBt *)user_data;

    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_SIGNAL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *iface = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);
    if (!iface || !member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (strcmp(iface, "org.freedesktop.DBus.Properties") == 0 &&
        strcmp(member, "PropertiesChanged") == 0) {
        handle_properties_changed(tb, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (strcmp(iface, BLUEZ_OBJMGR_IFACE) == 0 ||
        strcmp(iface, "org.freedesktop.DBus.ObjectManager") == 0) {
        if (strcmp(member, "InterfacesAdded") == 0) {
            handle_interfaces_added(tb, msg);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (strcmp(member, "InterfacesRemoved") == 0) {
            handle_interfaces_removed(tb, msg);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
    }

    if (strcmp(iface, "org.freedesktop.DBus") == 0 &&
        strcmp(member, "NameOwnerChanged") == 0) {
        const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
        if (dbus_message_get_args(msg, NULL,
                                   DBUS_TYPE_STRING, &name,
                                   DBUS_TYPE_STRING, &old_owner,
                                   DBUS_TYPE_STRING, &new_owner,
                                   DBUS_TYPE_INVALID)) {
            if (strcmp(name, BLUEZ_SERVICE) == 0) {
                if (new_owner && new_owner[0]) {
                    fprintf(stderr, "isde-tray-bt: BlueZ appeared\n");
                    tb->bluez_available = 1;
                    tb_bluez_refresh(tb);
                    tray_bt_update_icon(tb);
                    tb_menu_rebuild(tb);
                } else {
                    fprintf(stderr, "isde-tray-bt: BlueZ disappeared\n");
                    tb->bluez_available = 0;
                    tb->has_adapter = 0;
                    tb->ndevices = 0;
                    memset(&tb->adapter, 0, sizeof(tb->adapter));
                    tray_bt_update_icon(tb);
                    tb_menu_rebuild(tb);
                }
            }
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ---------- init / cleanup ---------- */

int tb_bluez_init(TrayBt *tb)
{
    DBusError err;
    dbus_error_init(&err);

    tb->system_bus = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!tb->system_bus) {
        fprintf(stderr, "isde-tray-bt: system bus: %s\n",
                err.message ? err.message : "cannot connect");
        dbus_error_free(&err);
        return -1;
    }

    dbus_connection_set_exit_on_disconnect(tb->system_bus, FALSE);

    dbus_bus_add_match(tb->system_bus,
        "type='signal',sender='" BLUEZ_SERVICE "',"
        "interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged'", &err);
    dbus_bus_add_match(tb->system_bus,
        "type='signal',sender='" BLUEZ_SERVICE "',"
        "interface='org.freedesktop.DBus.ObjectManager'", NULL);

    dbus_bus_add_match(tb->system_bus,
        "type='signal',sender='org.freedesktop.DBus',"
        "interface='org.freedesktop.DBus',"
        "member='NameOwnerChanged',"
        "arg0='" BLUEZ_SERVICE "'", NULL);

    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "isde-tray-bt: D-Bus match: %s\n", err.message);
        dbus_error_free(&err);
    }

    dbus_connection_add_filter(tb->system_bus, signal_filter, tb, NULL);

    dbus_error_free(&err);
    return 0;
}

void tb_bluez_cleanup(TrayBt *tb)
{
    if (tb->system_bus) {
        dbus_connection_remove_filter(tb->system_bus, signal_filter, tb);
        dbus_connection_unref(tb->system_bus);
        tb->system_bus = NULL;
    }
}

/* ---------- GetManagedObjects ---------- */

static DBusMessage *get_managed_objects(TrayBt *tb)
{
    DBusMessage *msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, "/",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    if (!msg)
        return NULL;

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        tb->system_bus, msg, 2000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        if (dbus_error_is_set(&err))
            dbus_error_free(&err);
        return NULL;
    }

    return reply;
}

int tb_bluez_get_adapter(TrayBt *tb)
{
    if (!tb->system_bus)
        return -1;

    DBusMessage *reply = get_managed_objects(tb);
    if (!reply)
        return -1;

    tb->has_adapter = 0;
    memset(&tb->adapter, 0, sizeof(tb->adapter));

    DBusMessageIter iter, objects;
    if (!dbus_message_iter_init(reply, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return -1;
    }

    dbus_message_iter_recurse(&iter, &objects);

    while (dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter obj_entry;
        dbus_message_iter_recurse(&objects, &obj_entry);

        const char *path;
        dbus_message_iter_get_basic(&obj_entry, &path);
        dbus_message_iter_next(&obj_entry);

        DBusMessageIter ifaces;
        dbus_message_iter_recurse(&obj_entry, &ifaces);

        while (dbus_message_iter_get_arg_type(&ifaces) ==
               DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter iface_entry;
            dbus_message_iter_recurse(&ifaces, &iface_entry);

            const char *iface;
            dbus_message_iter_get_basic(&iface_entry, &iface);

            if (strcmp(iface, BLUEZ_ADAPTER_IFACE) == 0 && !tb->has_adapter) {
                dbus_message_iter_next(&iface_entry);
                DBusMessageIter props;
                dbus_message_iter_recurse(&iface_entry, &props);

                snprintf(tb->adapter.path, sizeof(tb->adapter.path),
                         "%s", path);
                parse_adapter_properties(&tb->adapter, &props);
                tb->has_adapter = 1;
            }

            dbus_message_iter_next(&ifaces);
        }

        if (tb->has_adapter)
            break;

        dbus_message_iter_next(&objects);
    }

    dbus_message_unref(reply);
    return tb->has_adapter ? 0 : -1;
}

int tb_bluez_get_devices(TrayBt *tb)
{
    if (!tb->system_bus)
        return -1;

    DBusMessage *reply = get_managed_objects(tb);
    if (!reply)
        return -1;

    tb->ndevices = 0;

    DBusMessageIter iter, objects;
    if (!dbus_message_iter_init(reply, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return -1;
    }

    dbus_message_iter_recurse(&iter, &objects);

    while (dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY) {
        if (tb->ndevices >= MAX_DEVICES)
            break;

        DBusMessageIter obj_entry;
        dbus_message_iter_recurse(&objects, &obj_entry);

        const char *path;
        dbus_message_iter_get_basic(&obj_entry, &path);
        dbus_message_iter_next(&obj_entry);

        if (!path_is_device(path)) {
            dbus_message_iter_next(&objects);
            continue;
        }

        DBusMessageIter ifaces;
        dbus_message_iter_recurse(&obj_entry, &ifaces);

        while (dbus_message_iter_get_arg_type(&ifaces) ==
               DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter iface_entry;
            dbus_message_iter_recurse(&ifaces, &iface_entry);

            const char *iface;
            dbus_message_iter_get_basic(&iface_entry, &iface);

            if (strcmp(iface, BLUEZ_DEVICE_IFACE) == 0) {
                dbus_message_iter_next(&iface_entry);
                DBusMessageIter props;
                dbus_message_iter_recurse(&iface_entry, &props);

                DeviceInfo *d = &tb->devices[tb->ndevices];
                memset(d, 0, sizeof(*d));
                snprintf(d->path, sizeof(d->path), "%s", path);
                parse_device_properties(d, &props);
                tb->ndevices++;
                break;
            }

            dbus_message_iter_next(&ifaces);
        }

        dbus_message_iter_next(&objects);
    }

    dbus_message_unref(reply);
    return 0;
}

/* ---------- refresh all ---------- */

int tb_bluez_refresh(TrayBt *tb)
{
    if (tb_bluez_get_adapter(tb) != 0)
        return -1;
    tb_bluez_get_devices(tb);
    return 0;
}

/* ---------- Adapter power ---------- */

int tb_bluez_set_powered(TrayBt *tb, int powered)
{
    if (!tb->system_bus || !tb->has_adapter)
        return -1;

    DBusMessage *msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, tb->adapter.path,
        BLUEZ_PROPS_IFACE, "Set");
    if (!msg)
        return -1;

    const char *iface = BLUEZ_ADAPTER_IFACE;
    const char *prop = "Powered";
    dbus_bool_t val = powered ? TRUE : FALSE;

    DBusMessageIter iter, variant;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &prop);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT,
                                     DBUS_TYPE_BOOLEAN_AS_STRING, &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
    dbus_message_iter_close_container(&iter, &variant);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        tb->system_bus, msg, 5000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        if (dbus_error_is_set(&err)) {
            fprintf(stderr, "isde-tray-bt: set powered: %s\n", err.message);
            dbus_error_free(&err);
        }
        return -1;
    }

    dbus_message_unref(reply);
    dbus_error_free(&err);
    return 0;
}

/* ---------- Discovery ---------- */

static void async_reply_cb(DBusPendingCall *pending, void *user_data)
{
    const char *action = (const char *)user_data;
    DBusMessage *reply = dbus_pending_call_steal_reply(pending);
    if (reply) {
        if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
            const char *err = dbus_message_get_error_name(reply);
            fprintf(stderr, "isde-tray-bt: %s failed: %s\n",
                    action, err ? err : "unknown error");
        }
        dbus_message_unref(reply);
    }
    dbus_pending_call_unref(pending);
}

static int send_adapter_method(TrayBt *tb, const char *method)
{
    if (!tb->system_bus || !tb->has_adapter)
        return -1;

    DBusMessage *msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, tb->adapter.path,
        BLUEZ_ADAPTER_IFACE, method);
    if (!msg)
        return -1;

    DBusPendingCall *pending = NULL;
    if (!dbus_connection_send_with_reply(tb->system_bus, msg, &pending,
                                         30000)) {
        dbus_message_unref(msg);
        return -1;
    }
    dbus_message_unref(msg);

    if (pending)
        dbus_pending_call_set_notify(pending, async_reply_cb,
                                     (void *)method, NULL);
    return 0;
}

int tb_bluez_start_discovery(TrayBt *tb)
{
    return send_adapter_method(tb, "StartDiscovery");
}

int tb_bluez_stop_discovery(TrayBt *tb)
{
    return send_adapter_method(tb, "StopDiscovery");
}

/* ---------- Device methods ---------- */

static int send_device_method(TrayBt *tb, const char *path,
                              const char *method)
{
    if (!tb->system_bus)
        return -1;

    DBusMessage *msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, path, BLUEZ_DEVICE_IFACE, method);
    if (!msg)
        return -1;

    DBusPendingCall *pending = NULL;
    if (!dbus_connection_send_with_reply(tb->system_bus, msg, &pending,
                                         30000)) {
        dbus_message_unref(msg);
        return -1;
    }
    dbus_message_unref(msg);

    if (pending)
        dbus_pending_call_set_notify(pending, async_reply_cb,
                                     (void *)method, NULL);
    return 0;
}

int tb_bluez_device_connect(TrayBt *tb, const char *path)
{
    return send_device_method(tb, path, "Connect");
}

int tb_bluez_device_disconnect(TrayBt *tb, const char *path)
{
    return send_device_method(tb, path, "Disconnect");
}

int tb_bluez_device_pair(TrayBt *tb, const char *path)
{
    return send_device_method(tb, path, "Pair");
}

/* ---------- Device trust ---------- */

int tb_bluez_device_trust(TrayBt *tb, const char *path)
{
    if (!tb->system_bus)
        return -1;

    DBusMessage *msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, path, BLUEZ_PROPS_IFACE, "Set");
    if (!msg)
        return -1;

    const char *iface = BLUEZ_DEVICE_IFACE;
    const char *prop = "Trusted";
    dbus_bool_t val = TRUE;

    DBusMessageIter iter, variant;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &prop);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT,
                                     DBUS_TYPE_BOOLEAN_AS_STRING, &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
    dbus_message_iter_close_container(&iter, &variant);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        tb->system_bus, msg, 5000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        if (dbus_error_is_set(&err)) {
            fprintf(stderr, "isde-tray-bt: trust device: %s\n", err.message);
            dbus_error_free(&err);
        }
        return -1;
    }

    dbus_message_unref(reply);
    dbus_error_free(&err);
    return 0;
}

/* ---------- Remove device ---------- */

int tb_bluez_device_remove(TrayBt *tb, const char *path)
{
    if (!tb->system_bus || !tb->has_adapter)
        return -1;

    DBusMessage *msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, tb->adapter.path,
        BLUEZ_ADAPTER_IFACE, "RemoveDevice");
    if (!msg)
        return -1;

    dbus_message_append_args(msg,
                             DBUS_TYPE_OBJECT_PATH, &path,
                             DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        tb->system_bus, msg, 5000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        if (dbus_error_is_set(&err)) {
            fprintf(stderr, "isde-tray-bt: remove device: %s\n", err.message);
            dbus_error_free(&err);
        }
        return -1;
    }

    dbus_message_unref(reply);
    dbus_error_free(&err);
    return 0;
}
