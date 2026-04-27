#define _POSIX_C_SOURCE 200809L
/*
 * agent.c — ConnMan Agent for PSK passphrase entry
 *
 * Registers a net.connman.Agent object on the system bus.
 * When ConnMan needs a passphrase it calls RequestInput;
 * we show a dialog, collect the passphrase, and reply.
 */
#include "tray-net.h"

#include <ISW/Text.h>
#include <ISW/TextSink.h>
#include <ISW/IswArgMacros.h>

#include <stdio.h>
#include <string.h>

/* ---------- reply helpers ---------- */

static void send_canceled(TrayNet *tn)
{
    if (!tn->pending_agent_req)
        return;

    DBusMessage *reply = dbus_message_new_error(
        tn->pending_agent_req,
        "net.connman.Error.Canceled", "User canceled");
    if (reply) {
        dbus_connection_send(tn->system_bus, reply, NULL);
        dbus_message_unref(reply);
    }
    dbus_message_unref(tn->pending_agent_req);
    tn->pending_agent_req = NULL;
}

static void send_passphrase(TrayNet *tn, const char *passphrase)
{
    if (!tn->pending_agent_req)
        return;

    DBusMessage *reply = dbus_message_new_method_return(tn->pending_agent_req);
    if (!reply)
        goto out;

    DBusMessageIter iter, dict, entry, variant;
    dbus_message_iter_init_append(reply, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);

    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char *key = "Passphrase";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                     DBUS_TYPE_STRING_AS_STRING, &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &passphrase);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&dict, &entry);

    dbus_message_iter_close_container(&iter, &dict);

    dbus_connection_send(tn->system_bus, reply, NULL);
    dbus_message_unref(reply);

out:
    dbus_message_unref(tn->pending_agent_req);
    tn->pending_agent_req = NULL;
}

/* ---------- dialog callback ---------- */

static void on_input_result(IsdeDialogResult result, const char *text,
                            void *data)
{
    TrayNet *tn = (TrayNet *)data;

    if (result == ISDE_DIALOG_OK && text && text[0])
        send_passphrase(tn, text);
    else
        send_canceled(tn);

    tn->agent_dialog = NULL;
}

/* ---------- find SSID for a service path ---------- */

static const char *ssid_for_path(TrayNet *tn, const char *path)
{
    for (int i = 0; i < tn->nservices; i++) {
        if (strcmp(tn->services[i].path, path) == 0)
            return tn->services[i].name[0] ? tn->services[i].name : NULL;
    }
    return NULL;
}

/* ---------- D-Bus object handler ---------- */

static DBusHandlerResult
agent_message_handler(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    (void)conn;
    TrayNet *tn = (TrayNet *)user_data;

    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *iface = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);
    if (!iface || !member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (strcmp(iface, CONNMAN_AGENT_IFACE) != 0)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (strcmp(member, "Release") == 0) {
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) {
            dbus_connection_send(tn->system_bus, reply, NULL);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (strcmp(member, "Cancel") == 0) {
        if (tn->agent_dialog) {
            isde_dialog_dismiss(tn->agent_dialog);
            tn->agent_dialog = NULL;
        }
        if (tn->pending_agent_req) {
            dbus_message_unref(tn->pending_agent_req);
            tn->pending_agent_req = NULL;
        }
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) {
            dbus_connection_send(tn->system_bus, reply, NULL);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (strcmp(member, "ReportError") == 0) {
        const char *path = NULL, *error = NULL;
        dbus_message_get_args(msg, NULL,
                              DBUS_TYPE_OBJECT_PATH, &path,
                              DBUS_TYPE_STRING, &error,
                              DBUS_TYPE_INVALID);
        fprintf(stderr, "isde-tray-net: agent error: %s: %s\n",
                path ? path : "?", error ? error : "?");
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) {
            dbus_connection_send(tn->system_bus, reply, NULL);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (strcmp(member, "RequestInput") == 0) {
        const char *svc_path = NULL;
        DBusMessageIter args_iter;
        if (!dbus_message_iter_init(msg, &args_iter))
            goto reject;

        if (dbus_message_iter_get_arg_type(&args_iter) != DBUS_TYPE_OBJECT_PATH)
            goto reject;
        dbus_message_iter_get_basic(&args_iter, &svc_path);
        dbus_message_iter_next(&args_iter);

        if (dbus_message_iter_get_arg_type(&args_iter) != DBUS_TYPE_ARRAY)
            goto reject;

        /* Check if Passphrase is in the requested fields */
        int wants_passphrase = 0;
        DBusMessageIter dict;
        dbus_message_iter_recurse(&args_iter, &dict);
        while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry;
            dbus_message_iter_recurse(&dict, &entry);
            const char *key;
            dbus_message_iter_get_basic(&entry, &key);
            if (strcmp(key, "Passphrase") == 0)
                wants_passphrase = 1;
            dbus_message_iter_next(&dict);
        }

        if (!wants_passphrase)
            goto reject;

        /* Cancel any previous pending request */
        if (tn->pending_agent_req)
            send_canceled(tn);
        if (tn->agent_dialog) {
            isde_dialog_dismiss(tn->agent_dialog);
            tn->agent_dialog = NULL;
        }

        /* Hold the message — we'll reply from the dialog callback */
        tn->pending_agent_req = dbus_message_ref(msg);

        const char *ssid = ssid_for_path(tn, svc_path);
        char prompt[512];
        snprintf(prompt, sizeof(prompt), "Enter passphrase for %s",
                 ssid ? ssid : "network");

        tn->agent_dialog = isde_dialog_input(
            tn->toplevel, "WiFi Passphrase", prompt, "",
            on_input_result, tn);

        /* Fix up the value text widget */
        Widget value_w = IswNameToWidget(tn->agent_dialog, "*value");
        if (value_w) {
            IswArgBuilder mab = IswArgBuilderInit();
            IswArgBuilderAdd(&mab, IswNresize, (IswArgVal)IswtextResizeNever);
            IswSetValues(value_w, mab.args, mab.count);

            /* Mask input */
            Widget sink = NULL;
            IswArgBuilderReset(&mab);
            IswArgBuilderAdd(&mab, IswNtextSink, (IswArgVal)&sink);
            IswGetValues(value_w, mab.args, mab.count);
            if (sink) {
                IswArgBuilderReset(&mab);
                IswArgBuilderAdd(&mab, IswNecho, (IswArgVal)False);
                IswSetValues(sink, mab.args, mab.count);
            }
        }

        return DBUS_HANDLER_RESULT_HANDLED;
    }

reject:
    {
        DBusMessage *reply = dbus_message_new_error(
            msg, "net.connman.Error.Canceled", "Not supported");
        if (reply) {
            dbus_connection_send(tn->system_bus, reply, NULL);
            dbus_message_unref(reply);
        }
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* ---------- vtable for object path ---------- */

static void agent_unregister(DBusConnection *conn, void *user_data)
{
    (void)conn; (void)user_data;
}

static DBusObjectPathVTable agent_vtable = {
    .unregister_function = agent_unregister,
    .message_function    = agent_message_handler,
};

/* ---------- public API ---------- */

int tn_agent_init(TrayNet *tn)
{
    if (!tn->system_bus)
        return -1;

    if (!dbus_connection_register_object_path(tn->system_bus,
                                               CONNMAN_AGENT_PATH,
                                               &agent_vtable, tn)) {
        fprintf(stderr, "isde-tray-net: failed to register agent path\n");
        return -1;
    }

    DBusMessage *msg = dbus_message_new_method_call(
        CONNMAN_SERVICE, CONNMAN_MANAGER_PATH,
        CONNMAN_MANAGER_IFACE, "RegisterAgent");
    if (!msg)
        return -1;

    const char *path = CONNMAN_AGENT_PATH;
    dbus_message_append_args(msg,
                             DBUS_TYPE_OBJECT_PATH, &path,
                             DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        tn->system_bus, msg, 5000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        if (dbus_error_is_set(&err))
            fprintf(stderr, "isde-tray-net: RegisterAgent: %s\n", err.message);
        dbus_error_free(&err);
        return -1;
    }

    dbus_message_unref(reply);
    dbus_error_free(&err);
    fprintf(stderr, "isde-tray-net: agent registered\n");
    return 0;
}

void tn_agent_cleanup(TrayNet *tn)
{
    if (tn->agent_dialog) {
        isde_dialog_dismiss(tn->agent_dialog);
        tn->agent_dialog = NULL;
    }

    if (tn->pending_agent_req) {
        send_canceled(tn);
    }

    if (tn->system_bus) {
        DBusMessage *msg = dbus_message_new_method_call(
            CONNMAN_SERVICE, CONNMAN_MANAGER_PATH,
            CONNMAN_MANAGER_IFACE, "UnregisterAgent");
        if (msg) {
            const char *path = CONNMAN_AGENT_PATH;
            dbus_message_append_args(msg,
                                     DBUS_TYPE_OBJECT_PATH, &path,
                                     DBUS_TYPE_INVALID);
            dbus_connection_send(tn->system_bus, msg, NULL);
            dbus_message_unref(msg);
        }

        dbus_connection_unregister_object_path(tn->system_bus,
                                                CONNMAN_AGENT_PATH);
    }
}
