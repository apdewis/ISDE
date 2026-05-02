#define _POSIX_C_SOURCE 200809L
/*
 * agent.c — BlueZ pairing agent
 *
 * Registers a org.bluez.Agent1 object on the system bus.
 * Handles pairing requests: PIN code, passkey, confirmation.
 */
#include "tray-bt.h"

#include <ISW/IswArgMacros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- reply helpers ---------- */

static void send_canceled(TrayBt *tb)
{
    if (!tb->pending_agent_req)
        return;

    DBusMessage *reply = dbus_message_new_error(
        tb->pending_agent_req,
        "org.bluez.Error.Canceled", "User canceled");
    if (reply) {
        dbus_connection_send(tb->system_bus, reply, NULL);
        dbus_message_unref(reply);
    }
    dbus_message_unref(tb->pending_agent_req);
    tb->pending_agent_req = NULL;
}

static void send_rejected(TrayBt *tb)
{
    if (!tb->pending_agent_req)
        return;

    DBusMessage *reply = dbus_message_new_error(
        tb->pending_agent_req,
        "org.bluez.Error.Rejected", "Rejected");
    if (reply) {
        dbus_connection_send(tb->system_bus, reply, NULL);
        dbus_message_unref(reply);
    }
    dbus_message_unref(tb->pending_agent_req);
    tb->pending_agent_req = NULL;
}

static void send_pincode(TrayBt *tb, const char *pin)
{
    if (!tb->pending_agent_req)
        return;

    DBusMessage *reply = dbus_message_new_method_return(tb->pending_agent_req);
    if (reply) {
        dbus_message_append_args(reply,
                                 DBUS_TYPE_STRING, &pin,
                                 DBUS_TYPE_INVALID);
        dbus_connection_send(tb->system_bus, reply, NULL);
        dbus_message_unref(reply);
    }
    dbus_message_unref(tb->pending_agent_req);
    tb->pending_agent_req = NULL;
}

static void send_passkey(TrayBt *tb, uint32_t passkey)
{
    if (!tb->pending_agent_req)
        return;

    DBusMessage *reply = dbus_message_new_method_return(tb->pending_agent_req);
    if (reply) {
        dbus_message_append_args(reply,
                                 DBUS_TYPE_UINT32, &passkey,
                                 DBUS_TYPE_INVALID);
        dbus_connection_send(tb->system_bus, reply, NULL);
        dbus_message_unref(reply);
    }
    dbus_message_unref(tb->pending_agent_req);
    tb->pending_agent_req = NULL;
}

static void send_empty_reply(TrayBt *tb)
{
    if (!tb->pending_agent_req)
        return;

    DBusMessage *reply = dbus_message_new_method_return(tb->pending_agent_req);
    if (reply) {
        dbus_connection_send(tb->system_bus, reply, NULL);
        dbus_message_unref(reply);
    }
    dbus_message_unref(tb->pending_agent_req);
    tb->pending_agent_req = NULL;
}

/* ---------- find device name for path ---------- */

static const char *name_for_device(TrayBt *tb, const char *path)
{
    for (int i = 0; i < tb->ndevices; i++) {
        if (strcmp(tb->devices[i].path, path) == 0)
            return tb->devices[i].name[0] ? tb->devices[i].name : NULL;
    }
    return NULL;
}

/* ---------- dialog callbacks ---------- */

static void on_pincode_result(IsdeDialogResult result, const char *text,
                              void *data)
{
    TrayBt *tb = (TrayBt *)data;

    if (result == ISDE_DIALOG_OK && text && text[0])
        send_pincode(tb, text);
    else
        send_canceled(tb);

    tb->agent_dialog = NULL;
}

static void on_passkey_result(IsdeDialogResult result, const char *text,
                              void *data)
{
    TrayBt *tb = (TrayBt *)data;

    if (result == ISDE_DIALOG_OK && text && text[0]) {
        uint32_t passkey = (uint32_t)strtoul(text, NULL, 10);
        send_passkey(tb, passkey);
    } else {
        send_canceled(tb);
    }

    tb->agent_dialog = NULL;
}

static void on_confirm_result(IsdeDialogResult result, void *data)
{
    TrayBt *tb = (TrayBt *)data;

    if (result == ISDE_DIALOG_OK)
        send_empty_reply(tb);
    else
        send_rejected(tb);

    tb->agent_dialog = NULL;
}

static void on_display_dismissed(IsdeDialogResult result, void *data)
{
    (void)result;
    TrayBt *tb = (TrayBt *)data;
    tb->agent_dialog = NULL;
}

/* ---------- dismiss any pending agent dialog ---------- */

static void dismiss_agent_dialog(TrayBt *tb)
{
    if (tb->agent_dialog) {
        isde_dialog_dismiss(tb->agent_dialog);
        tb->agent_dialog = NULL;
    }
}

/* ---------- D-Bus object handler ---------- */

static DBusHandlerResult
agent_message_handler(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    (void)conn;
    TrayBt *tb = (TrayBt *)user_data;

    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *iface = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);
    if (!iface || !member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (strcmp(iface, BLUEZ_AGENT_IFACE) != 0)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (strcmp(member, "Release") == 0) {
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) {
            dbus_connection_send(tb->system_bus, reply, NULL);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (strcmp(member, "Cancel") == 0) {
        dismiss_agent_dialog(tb);
        if (tb->pending_agent_req) {
            dbus_message_unref(tb->pending_agent_req);
            tb->pending_agent_req = NULL;
        }
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) {
            dbus_connection_send(tb->system_bus, reply, NULL);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (strcmp(member, "RequestPinCode") == 0) {
        const char *dev_path = NULL;
        if (!dbus_message_get_args(msg, NULL,
                                    DBUS_TYPE_OBJECT_PATH, &dev_path,
                                    DBUS_TYPE_INVALID))
            goto reject;

        if (tb->pending_agent_req)
            send_canceled(tb);
        dismiss_agent_dialog(tb);

        tb->pending_agent_req = dbus_message_ref(msg);

        const char *name = name_for_device(tb, dev_path);
        char prompt[512];
        snprintf(prompt, sizeof(prompt), "Enter PIN for %s",
                 name ? name : "device");

        tb->agent_dialog = isde_dialog_input(
            tb->toplevel, "Bluetooth Pairing", prompt, "",
            on_pincode_result, tb);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (strcmp(member, "RequestPasskey") == 0) {
        const char *dev_path = NULL;
        if (!dbus_message_get_args(msg, NULL,
                                    DBUS_TYPE_OBJECT_PATH, &dev_path,
                                    DBUS_TYPE_INVALID))
            goto reject;

        if (tb->pending_agent_req)
            send_canceled(tb);
        dismiss_agent_dialog(tb);

        tb->pending_agent_req = dbus_message_ref(msg);

        const char *name = name_for_device(tb, dev_path);
        char prompt[512];
        snprintf(prompt, sizeof(prompt), "Enter passkey for %s",
                 name ? name : "device");

        tb->agent_dialog = isde_dialog_input(
            tb->toplevel, "Bluetooth Pairing", prompt, "",
            on_passkey_result, tb);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (strcmp(member, "DisplayPasskey") == 0) {
        const char *dev_path = NULL;
        uint32_t passkey = 0;
        uint16_t entered = 0;
        if (!dbus_message_get_args(msg, NULL,
                                    DBUS_TYPE_OBJECT_PATH, &dev_path,
                                    DBUS_TYPE_UINT32, &passkey,
                                    DBUS_TYPE_UINT16, &entered,
                                    DBUS_TYPE_INVALID)) {
            DBusMessage *reply = dbus_message_new_method_return(msg);
            if (reply) {
                dbus_connection_send(tb->system_bus, reply, NULL);
                dbus_message_unref(reply);
            }
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        dismiss_agent_dialog(tb);

        const char *name = name_for_device(tb, dev_path);
        char message[512];
        snprintf(message, sizeof(message),
                 "Enter %06u on %s", passkey,
                 name ? name : "device");

        tb->agent_dialog = isde_dialog_message(
            tb->toplevel, "Bluetooth Pairing", message,
            on_display_dismissed, tb);

        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) {
            dbus_connection_send(tb->system_bus, reply, NULL);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (strcmp(member, "RequestConfirmation") == 0) {
        const char *dev_path = NULL;
        uint32_t passkey = 0;
        if (!dbus_message_get_args(msg, NULL,
                                    DBUS_TYPE_OBJECT_PATH, &dev_path,
                                    DBUS_TYPE_UINT32, &passkey,
                                    DBUS_TYPE_INVALID))
            goto reject;

        if (tb->pending_agent_req)
            send_canceled(tb);
        dismiss_agent_dialog(tb);

        tb->pending_agent_req = dbus_message_ref(msg);

        const char *name = name_for_device(tb, dev_path);
        char message[512];
        snprintf(message, sizeof(message),
                 "Confirm passkey %06u for %s?", passkey,
                 name ? name : "device");

        tb->agent_dialog = isde_dialog_confirm(
            tb->toplevel, "Bluetooth Pairing", message, "Confirm",
            on_confirm_result, tb);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (strcmp(member, "AuthorizeService") == 0) {
        const char *dev_path = NULL;
        const char *uuid = NULL;
        if (!dbus_message_get_args(msg, NULL,
                                    DBUS_TYPE_OBJECT_PATH, &dev_path,
                                    DBUS_TYPE_STRING, &uuid,
                                    DBUS_TYPE_INVALID))
            goto reject;

        /* Auto-accept for trusted devices */
        for (int i = 0; i < tb->ndevices; i++) {
            if (strcmp(tb->devices[i].path, dev_path) == 0 &&
                tb->devices[i].trusted) {
                DBusMessage *reply = dbus_message_new_method_return(msg);
                if (reply) {
                    dbus_connection_send(tb->system_bus, reply, NULL);
                    dbus_message_unref(reply);
                }
                return DBUS_HANDLER_RESULT_HANDLED;
            }
        }

        if (tb->pending_agent_req)
            send_canceled(tb);
        dismiss_agent_dialog(tb);

        tb->pending_agent_req = dbus_message_ref(msg);

        const char *name = name_for_device(tb, dev_path);
        char message[512];
        snprintf(message, sizeof(message),
                 "Authorize service for %s?",
                 name ? name : "device");

        tb->agent_dialog = isde_dialog_confirm(
            tb->toplevel, "Bluetooth Authorization", message, "Authorize",
            on_confirm_result, tb);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

reject:
    {
        DBusMessage *reply = dbus_message_new_error(
            msg, "org.bluez.Error.Rejected", "Not supported");
        if (reply) {
            dbus_connection_send(tb->system_bus, reply, NULL);
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

int tb_agent_init(TrayBt *tb)
{
    if (!tb->system_bus)
        return -1;

    if (!dbus_connection_register_object_path(tb->system_bus,
                                               BLUEZ_AGENT_PATH,
                                               &agent_vtable, tb)) {
        fprintf(stderr, "isde-tray-bt: failed to register agent path\n");
        return -1;
    }

    /* RegisterAgent */
    DBusMessage *msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, "/org/bluez",
        BLUEZ_AGENTMGR_IFACE, "RegisterAgent");
    if (!msg)
        return -1;

    const char *path = BLUEZ_AGENT_PATH;
    const char *capability = "KeyboardDisplay";
    dbus_message_append_args(msg,
                             DBUS_TYPE_OBJECT_PATH, &path,
                             DBUS_TYPE_STRING, &capability,
                             DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        tb->system_bus, msg, 5000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        if (dbus_error_is_set(&err))
            fprintf(stderr, "isde-tray-bt: RegisterAgent: %s\n", err.message);
        dbus_error_free(&err);
        return -1;
    }
    dbus_message_unref(reply);

    /* RequestDefaultAgent */
    msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, "/org/bluez",
        BLUEZ_AGENTMGR_IFACE, "RequestDefaultAgent");
    if (msg) {
        dbus_message_append_args(msg,
                                 DBUS_TYPE_OBJECT_PATH, &path,
                                 DBUS_TYPE_INVALID);

        dbus_error_init(&err);
        reply = dbus_connection_send_with_reply_and_block(
            tb->system_bus, msg, 5000, &err);
        dbus_message_unref(msg);

        if (!reply) {
            if (dbus_error_is_set(&err))
                fprintf(stderr, "isde-tray-bt: RequestDefaultAgent: %s\n",
                        err.message);
            dbus_error_free(&err);
        } else {
            dbus_message_unref(reply);
        }
    }

    dbus_error_free(&err);
    fprintf(stderr, "isde-tray-bt: agent registered\n");
    return 0;
}

void tb_agent_cleanup(TrayBt *tb)
{
    dismiss_agent_dialog(tb);

    if (tb->pending_agent_req)
        send_canceled(tb);

    if (tb->system_bus) {
        DBusMessage *msg = dbus_message_new_method_call(
            BLUEZ_SERVICE, "/org/bluez",
            BLUEZ_AGENTMGR_IFACE, "UnregisterAgent");
        if (msg) {
            const char *path = BLUEZ_AGENT_PATH;
            dbus_message_append_args(msg,
                                     DBUS_TYPE_OBJECT_PATH, &path,
                                     DBUS_TYPE_INVALID);
            dbus_connection_send(tb->system_bus, msg, NULL);
            dbus_message_unref(msg);
        }

        dbus_connection_unregister_object_path(tb->system_bus,
                                                BLUEZ_AGENT_PATH);
    }
}
