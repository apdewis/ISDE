#define _POSIX_C_SOURCE 200809L
/*
 * connman.c — D-Bus client for ConnMan
 *
 * Queries technologies, services, and manager state via the system bus.
 * Subscribes to ConnMan signals for live state updates.
 */
#include "tray-net.h"

#include <stdio.h>
#include <string.h>

/* ---------- helpers: extract properties from variant iterators ---------- */

static void parse_tech_properties(TechInfo *t, DBusMessageIter *dict)
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
            snprintf(t->name, sizeof(t->name), "%s", v);
        } else if (strcmp(key, "Type") == 0 && vtype == DBUS_TYPE_STRING) {
            const char *v;
            dbus_message_iter_get_basic(&variant, &v);
            snprintf(t->type, sizeof(t->type), "%s", v);
        } else if (strcmp(key, "Powered") == 0 && vtype == DBUS_TYPE_BOOLEAN) {
            dbus_bool_t v;
            dbus_message_iter_get_basic(&variant, &v);
            t->powered = v;
        } else if (strcmp(key, "Connected") == 0 && vtype == DBUS_TYPE_BOOLEAN) {
            dbus_bool_t v;
            dbus_message_iter_get_basic(&variant, &v);
            t->connected = v;
        }

        dbus_message_iter_next(dict);
    }
}

static void parse_service_properties(ServiceInfo *s, DBusMessageIter *dict)
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
            snprintf(s->name, sizeof(s->name), "%s", v);
        } else if (strcmp(key, "Type") == 0 && vtype == DBUS_TYPE_STRING) {
            const char *v;
            dbus_message_iter_get_basic(&variant, &v);
            snprintf(s->type, sizeof(s->type), "%s", v);
        } else if (strcmp(key, "State") == 0 && vtype == DBUS_TYPE_STRING) {
            const char *v;
            dbus_message_iter_get_basic(&variant, &v);
            snprintf(s->state, sizeof(s->state), "%s", v);
        } else if (strcmp(key, "Security") == 0 && vtype == DBUS_TYPE_ARRAY) {
            /* Security is an array of strings; take the first */
            DBusMessageIter arr;
            dbus_message_iter_recurse(&variant, &arr);
            if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
                const char *v;
                dbus_message_iter_get_basic(&arr, &v);
                snprintf(s->security, sizeof(s->security), "%s", v);
            }
        } else if (strcmp(key, "Ethernet") == 0 && vtype == DBUS_TYPE_ARRAY) {
            DBusMessageIter eth_dict;
            dbus_message_iter_recurse(&variant, &eth_dict);
            while (dbus_message_iter_get_arg_type(&eth_dict) ==
                   DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter eth_entry, eth_var;
                dbus_message_iter_recurse(&eth_dict, &eth_entry);
                const char *ekey;
                dbus_message_iter_get_basic(&eth_entry, &ekey);
                dbus_message_iter_next(&eth_entry);
                dbus_message_iter_recurse(&eth_entry, &eth_var);
                if (strcmp(ekey, "Interface") == 0 &&
                    dbus_message_iter_get_arg_type(&eth_var) ==
                    DBUS_TYPE_STRING) {
                    const char *v;
                    dbus_message_iter_get_basic(&eth_var, &v);
                    snprintf(s->interface, sizeof(s->interface), "%s", v);
                }
                dbus_message_iter_next(&eth_dict);
            }
        } else if (strcmp(key, "Error") == 0 && vtype == DBUS_TYPE_STRING) {
            const char *v;
            dbus_message_iter_get_basic(&variant, &v);
            snprintf(s->error, sizeof(s->error), "%s", v);
        } else if (strcmp(key, "Strength") == 0 && vtype == DBUS_TYPE_BYTE) {
            uint8_t v;
            dbus_message_iter_get_basic(&variant, &v);
            s->strength = v;
        } else if (strcmp(key, "Favorite") == 0 && vtype == DBUS_TYPE_BOOLEAN) {
            dbus_bool_t v;
            dbus_message_iter_get_basic(&variant, &v);
            s->favorite = v;
        } else if (strcmp(key, "AutoConnect") == 0 && vtype == DBUS_TYPE_BOOLEAN) {
            dbus_bool_t v;
            dbus_message_iter_get_basic(&variant, &v);
            s->autoconnect = v;
        }

        dbus_message_iter_next(dict);
    }
}

/* ---------- signal filter ---------- */

static ServiceInfo *find_service_by_path(TrayNet *tn, const char *path)
{
    for (int i = 0; i < tn->nservices; i++) {
        if (strcmp(tn->services[i].path, path) == 0)
            return &tn->services[i];
    }
    return NULL;
}

static TechInfo *find_tech_by_path(TrayNet *tn, const char *path)
{
    for (int i = 0; i < tn->ntechs; i++) {
        if (strcmp(tn->techs[i].path, path) == 0)
            return &tn->techs[i];
    }
    return NULL;
}

static DBusHandlerResult
signal_filter(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    (void)conn;
    TrayNet *tn = (TrayNet *)user_data;

    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_SIGNAL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *iface = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);
    const char *path = dbus_message_get_path(msg);
    if (!iface || !member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (strcmp(iface, CONNMAN_MANAGER_IFACE) == 0) {
        if (strcmp(member, "PropertyChanged") == 0) {
            DBusMessageIter iter, variant;
            if (dbus_message_iter_init(msg, &iter)) {
                const char *key;
                dbus_message_iter_get_basic(&iter, &key);
                dbus_message_iter_next(&iter);
                dbus_message_iter_recurse(&iter, &variant);

                if (strcmp(key, "State") == 0) {
                    const char *v;
                    dbus_message_iter_get_basic(&variant, &v);
                    snprintf(tn->manager_state, sizeof(tn->manager_state),
                             "%s", v);
                    tray_net_update_icon(tn);
                    tn_menu_rebuild(tn);
                }
            }
        } else if (strcmp(member, "ServicesChanged") == 0) {
            tn_connman_get_services(tn);
            tray_net_update_icon(tn);
            tn_menu_rebuild(tn);
        } else if (strcmp(member, "TechnologyAdded") == 0 ||
                   strcmp(member, "TechnologyRemoved") == 0) {
            tn_connman_get_technologies(tn);
            tn_menu_rebuild(tn);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (strcmp(iface, CONNMAN_SERVICE_IFACE) == 0 &&
        strcmp(member, "PropertyChanged") == 0 && path) {
        ServiceInfo *svc = find_service_by_path(tn, path);
        if (svc) {
            DBusMessageIter iter, variant;
            if (dbus_message_iter_init(msg, &iter)) {
                const char *key;
                dbus_message_iter_get_basic(&iter, &key);
                dbus_message_iter_next(&iter);
                dbus_message_iter_recurse(&iter, &variant);
                int vtype = dbus_message_iter_get_arg_type(&variant);

                if (strcmp(key, "State") == 0 && vtype == DBUS_TYPE_STRING) {
                    const char *v;
                    dbus_message_iter_get_basic(&variant, &v);
                    snprintf(svc->state, sizeof(svc->state), "%s", v);
                    tray_net_update_icon(tn);
                    tn_menu_rebuild(tn);
                } else if (strcmp(key, "Strength") == 0 &&
                           vtype == DBUS_TYPE_BYTE) {
                    uint8_t v;
                    dbus_message_iter_get_basic(&variant, &v);
                    svc->strength = v;
                    tray_net_update_icon(tn);
                    tn_menu_rebuild(tn);
                } else if (strcmp(key, "Error") == 0 &&
                           vtype == DBUS_TYPE_STRING) {
                    const char *v;
                    dbus_message_iter_get_basic(&variant, &v);
                    snprintf(svc->error, sizeof(svc->error), "%s", v);
                    tn_menu_rebuild(tn);
                }
            }
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (strcmp(iface, CONNMAN_TECH_IFACE) == 0 &&
        strcmp(member, "PropertyChanged") == 0 && path) {
        TechInfo *tech = find_tech_by_path(tn, path);
        if (tech) {
            DBusMessageIter iter, variant;
            if (dbus_message_iter_init(msg, &iter)) {
                const char *key;
                dbus_message_iter_get_basic(&iter, &key);
                dbus_message_iter_next(&iter);
                dbus_message_iter_recurse(&iter, &variant);
                int vtype = dbus_message_iter_get_arg_type(&variant);

                if (strcmp(key, "Powered") == 0 &&
                    vtype == DBUS_TYPE_BOOLEAN) {
                    dbus_bool_t v;
                    dbus_message_iter_get_basic(&variant, &v);
                    tech->powered = v;
                    tn_connman_get_services(tn);
                    tray_net_update_icon(tn);
                    tn_menu_rebuild(tn);
                } else if (strcmp(key, "Connected") == 0 &&
                           vtype == DBUS_TYPE_BOOLEAN) {
                    dbus_bool_t v;
                    dbus_message_iter_get_basic(&variant, &v);
                    tech->connected = v;
                    tray_net_update_icon(tn);
                    tn_menu_rebuild(tn);
                }
            }
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* NameOwnerChanged for ConnMan appearing/disappearing */
    if (strcmp(iface, "org.freedesktop.DBus") == 0 &&
        strcmp(member, "NameOwnerChanged") == 0) {
        const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
        if (dbus_message_get_args(msg, NULL,
                                   DBUS_TYPE_STRING, &name,
                                   DBUS_TYPE_STRING, &old_owner,
                                   DBUS_TYPE_STRING, &new_owner,
                                   DBUS_TYPE_INVALID)) {
            if (strcmp(name, CONNMAN_SERVICE) == 0) {
                if (new_owner && new_owner[0]) {
                    fprintf(stderr, "isde-tray-net: ConnMan appeared\n");
                    tn->connman_available = 1;
                    tn_connman_refresh(tn);
                    tray_net_update_icon(tn);
                    tn_menu_rebuild(tn);
                } else {
                    fprintf(stderr, "isde-tray-net: ConnMan disappeared\n");
                    tn->connman_available = 0;
                    tn->nservices = 0;
                    tn->ntechs = 0;
                    snprintf(tn->manager_state,
                             sizeof(tn->manager_state), "idle");
                    tray_net_update_icon(tn);
                    tn_menu_rebuild(tn);
                }
            }
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ---------- init / cleanup ---------- */

int tn_connman_init(TrayNet *tn)
{
    DBusError err;
    dbus_error_init(&err);

    tn->system_bus = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!tn->system_bus) {
        fprintf(stderr, "isde-tray-net: system bus: %s\n",
                err.message ? err.message : "cannot connect");
        dbus_error_free(&err);
        return -1;
    }

    dbus_connection_set_exit_on_disconnect(tn->system_bus, FALSE);

    /* Subscribe to ConnMan signals */
    dbus_bus_add_match(tn->system_bus,
        "type='signal',sender='" CONNMAN_SERVICE "',"
        "interface='" CONNMAN_MANAGER_IFACE "'", &err);
    dbus_bus_add_match(tn->system_bus,
        "type='signal',sender='" CONNMAN_SERVICE "',"
        "interface='" CONNMAN_SERVICE_IFACE "'", NULL);
    dbus_bus_add_match(tn->system_bus,
        "type='signal',sender='" CONNMAN_SERVICE "',"
        "interface='" CONNMAN_TECH_IFACE "'", NULL);

    /* Watch for ConnMan appearing/disappearing */
    dbus_bus_add_match(tn->system_bus,
        "type='signal',sender='org.freedesktop.DBus',"
        "interface='org.freedesktop.DBus',"
        "member='NameOwnerChanged',"
        "arg0='" CONNMAN_SERVICE "'", NULL);

    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "isde-tray-net: D-Bus match: %s\n", err.message);
        dbus_error_free(&err);
    }

    dbus_connection_add_filter(tn->system_bus, signal_filter, tn, NULL);

    dbus_error_free(&err);
    return 0;
}

void tn_connman_cleanup(TrayNet *tn)
{
    if (tn->system_bus) {
        dbus_connection_remove_filter(tn->system_bus, signal_filter, tn);
        dbus_connection_unref(tn->system_bus);
        tn->system_bus = NULL;
    }
}

/* ---------- GetTechnologies ---------- */

int tn_connman_get_technologies(TrayNet *tn)
{
    if (!tn->system_bus)
        return -1;

    DBusMessage *msg = dbus_message_new_method_call(
        CONNMAN_SERVICE, CONNMAN_MANAGER_PATH,
        CONNMAN_MANAGER_IFACE, "GetTechnologies");
    if (!msg)
        return -1;

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        tn->system_bus, msg, 2000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        if (dbus_error_is_set(&err))
            dbus_error_free(&err);
        return -1;
    }

    tn->ntechs = 0;

    DBusMessageIter iter, array;
    if (!dbus_message_iter_init(reply, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return -1;
    }

    dbus_message_iter_recurse(&iter, &array);

    while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRUCT) {
        if (tn->ntechs >= MAX_TECHNOLOGIES)
            break;

        DBusMessageIter st;
        dbus_message_iter_recurse(&array, &st);

        TechInfo *t = &tn->techs[tn->ntechs];
        memset(t, 0, sizeof(*t));

        /* Object path */
        const char *p;
        dbus_message_iter_get_basic(&st, &p);
        snprintf(t->path, sizeof(t->path), "%s", p);
        dbus_message_iter_next(&st);

        /* Properties dict */
        DBusMessageIter dict;
        dbus_message_iter_recurse(&st, &dict);
        parse_tech_properties(t, &dict);

        tn->ntechs++;
        dbus_message_iter_next(&array);
    }

    dbus_message_unref(reply);
    return 0;
}

/* ---------- GetServices ---------- */

int tn_connman_get_services(TrayNet *tn)
{
    if (!tn->system_bus)
        return -1;

    DBusMessage *msg = dbus_message_new_method_call(
        CONNMAN_SERVICE, CONNMAN_MANAGER_PATH,
        CONNMAN_MANAGER_IFACE, "GetServices");
    if (!msg)
        return -1;

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        tn->system_bus, msg, 2000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        if (dbus_error_is_set(&err))
            dbus_error_free(&err);
        return -1;
    }

    tn->nservices = 0;

    DBusMessageIter iter, array;
    if (!dbus_message_iter_init(reply, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return -1;
    }

    dbus_message_iter_recurse(&iter, &array);

    while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRUCT) {
        if (tn->nservices >= MAX_SERVICES)
            break;

        DBusMessageIter st;
        dbus_message_iter_recurse(&array, &st);

        ServiceInfo *s = &tn->services[tn->nservices];
        memset(s, 0, sizeof(*s));

        const char *p;
        dbus_message_iter_get_basic(&st, &p);
        snprintf(s->path, sizeof(s->path), "%s", p);
        dbus_message_iter_next(&st);

        DBusMessageIter dict;
        dbus_message_iter_recurse(&st, &dict);
        parse_service_properties(s, &dict);

        tn->nservices++;
        dbus_message_iter_next(&array);
    }

    dbus_message_unref(reply);
    return 0;
}

/* ---------- GetProperties (manager state) ---------- */

int tn_connman_get_state(TrayNet *tn)
{
    if (!tn->system_bus)
        return -1;

    DBusMessage *msg = dbus_message_new_method_call(
        CONNMAN_SERVICE, CONNMAN_MANAGER_PATH,
        CONNMAN_MANAGER_IFACE, "GetProperties");
    if (!msg)
        return -1;

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        tn->system_bus, msg, 2000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        if (dbus_error_is_set(&err))
            dbus_error_free(&err);
        return -1;
    }

    DBusMessageIter iter, dict;
    if (!dbus_message_iter_init(reply, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return -1;
    }

    dbus_message_iter_recurse(&iter, &dict);

    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, variant;
        dbus_message_iter_recurse(&dict, &entry);

        const char *key;
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);
        dbus_message_iter_recurse(&entry, &variant);

        if (strcmp(key, "State") == 0 &&
            dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING) {
            const char *v;
            dbus_message_iter_get_basic(&variant, &v);
            snprintf(tn->manager_state, sizeof(tn->manager_state), "%s", v);
        }

        dbus_message_iter_next(&dict);
    }

    dbus_message_unref(reply);
    return 0;
}

/* ---------- refresh all ---------- */

int tn_connman_refresh(TrayNet *tn)
{
    if (tn_connman_get_state(tn) != 0)
        return -1;
    tn_connman_get_technologies(tn);
    tn_connman_get_services(tn);
    return 0;
}

/* ---------- Service.Connect / Disconnect ---------- */

static void async_reply_cb(DBusPendingCall *pending, void *user_data)
{
    const char *action = (const char *)user_data;
    DBusMessage *reply = dbus_pending_call_steal_reply(pending);
    if (reply) {
        if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
            const char *err = dbus_message_get_error_name(reply);
            fprintf(stderr, "isde-tray-net: %s failed: %s\n",
                    action, err ? err : "unknown error");
        }
        dbus_message_unref(reply);
    }
    dbus_pending_call_unref(pending);
}

static int send_service_method(TrayNet *tn, const char *path,
                               const char *method)
{
    if (!tn->system_bus)
        return -1;

    DBusMessage *msg = dbus_message_new_method_call(
        CONNMAN_SERVICE, path, CONNMAN_SERVICE_IFACE, method);
    if (!msg)
        return -1;

    DBusPendingCall *pending = NULL;
    if (!dbus_connection_send_with_reply(tn->system_bus, msg, &pending, 30000)) {
        dbus_message_unref(msg);
        return -1;
    }
    dbus_message_unref(msg);

    if (pending)
        dbus_pending_call_set_notify(pending, async_reply_cb,
                                     (void *)method, NULL);
    return 0;
}

int tn_connman_service_connect(TrayNet *tn, const char *path)
{
    return send_service_method(tn, path, "Connect");
}

int tn_connman_service_disconnect(TrayNet *tn, const char *path)
{
    return send_service_method(tn, path, "Disconnect");
}

/* ---------- Technology.SetProperty (Powered) ---------- */

int tn_connman_tech_set_powered(TrayNet *tn, const char *path, int powered)
{
    if (!tn->system_bus)
        return -1;

    DBusMessage *msg = dbus_message_new_method_call(
        CONNMAN_SERVICE, path, CONNMAN_TECH_IFACE, "SetProperty");
    if (!msg)
        return -1;

    const char *prop_name = "Powered";
    dbus_bool_t val = powered ? TRUE : FALSE;

    DBusMessageIter iter, variant;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &prop_name);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT,
                                     DBUS_TYPE_BOOLEAN_AS_STRING, &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
    dbus_message_iter_close_container(&iter, &variant);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        tn->system_bus, msg, 5000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        if (dbus_error_is_set(&err)) {
            fprintf(stderr, "isde-tray-net: set powered: %s\n", err.message);
            dbus_error_free(&err);
        }
        return -1;
    }

    dbus_message_unref(reply);
    dbus_error_free(&err);
    return 0;
}

/* ---------- Technology.Scan ---------- */

int tn_connman_scan(TrayNet *tn, const char *tech_path)
{
    if (!tn->system_bus)
        return -1;

    DBusMessage *msg = dbus_message_new_method_call(
        CONNMAN_SERVICE, tech_path, CONNMAN_TECH_IFACE, "Scan");
    if (!msg)
        return -1;

    /* Scan can be slow — send async */
    DBusPendingCall *pending = NULL;
    if (!dbus_connection_send_with_reply(tn->system_bus, msg, &pending, 30000)) {
        dbus_message_unref(msg);
        return -1;
    }

    dbus_message_unref(msg);
    if (pending)
        dbus_pending_call_unref(pending);
    return 0;
}
