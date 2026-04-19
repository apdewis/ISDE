#define _POSIX_C_SOURCE 200809L
/*
 * panel-dm.c — Display Manager settings: greeter clock format customization
 *
 * Communicates with isde-dm via D-Bus system bus (org.isde.DisplayManager).
 * Only greeter appearance settings (clock formats) are exposed here;
 * power permissions and lock timeout remain root-editable TOML only.
 */
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dbus/dbus.h>

#include <ISW/AsciiText.h>
#include <ISW/IswArgMacros.h>



static Widget text_time_fmt;
static Widget text_date_fmt;

static char saved_time_fmt[64];
static char saved_date_fmt[64];

/* D-Bus system bus connection (separate from session bus) */
static DBusConnection *sys_bus;

#define TEXT_W 300
#define LABEL_W 250

static void ensure_system_bus(void)
{
    if (sys_bus) {
        return;
    }
    DBusError err;
    dbus_error_init(&err);
    sys_bus = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!sys_bus) {
        fprintf(stderr, "isde-settings: DM panel: system bus: %s\n",
                err.message ? err.message : "cannot connect");
    }
    dbus_error_free(&err);
}

static void fetch_greeter_config(void)
{
    ensure_system_bus();
    if (!sys_bus) {
        return;
    }

    DBusMessage *msg = dbus_message_new_method_call(
        "org.isde.DisplayManager",
        "/org/isde/DisplayManager",
        "org.isde.DisplayManager",
        "GetGreeterConfig");
    if (!msg) {
        return;
    }

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        sys_bus, msg, 2000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        fprintf(stderr, "isde-settings: DM panel: GetGreeterConfig: %s\n",
                err.message ? err.message : "no reply");
        dbus_error_free(&err);
        return;
    }

    /* Parse a{ss} dict */
    DBusMessageIter iter, dict;
    if (dbus_message_iter_init(reply, &iter) &&
        dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&iter, &dict);
        while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry;
            dbus_message_iter_recurse(&dict, &entry);

            const char *key = NULL;
            const char *val = NULL;
            dbus_message_iter_get_basic(&entry, &key);
            dbus_message_iter_next(&entry);
            dbus_message_iter_get_basic(&entry, &val);

            if (key && val) {
                if (strcmp(key, "clock_time_format") == 0) {
                    snprintf(saved_time_fmt, sizeof(saved_time_fmt), "%s", val);
                } else if (strcmp(key, "clock_date_format") == 0) {
                    snprintf(saved_date_fmt, sizeof(saved_date_fmt), "%s", val);
                }
            }
            dbus_message_iter_next(&dict);
        }
    }

    dbus_message_unref(reply);
    dbus_error_free(&err);
}

static void set_greeter_config(const char *key, const char *value)
{
    ensure_system_bus();
    if (!sys_bus) {
        return;
    }

    DBusMessage *msg = dbus_message_new_method_call(
        "org.isde.DisplayManager",
        "/org/isde/DisplayManager",
        "org.isde.DisplayManager",
        "SetGreeterConfig");
    if (!msg) {
        return;
    }

    dbus_message_append_args(msg,
                             DBUS_TYPE_STRING, &key,
                             DBUS_TYPE_STRING, &value,
                             DBUS_TYPE_INVALID);
    dbus_connection_send(sys_bus, msg, NULL);
    dbus_connection_flush(sys_bus);
    dbus_message_unref(msg);
}

/* ---------- helpers ---------- */

static const char *get_text(Widget w)
{
    Arg args[20];
    String str = NULL;
    IswSetArg(args[0], IswNstring, &str);
    IswGetValues(w, args, 1);
    return str ? str : "";
}

static void set_text(Widget w, const char *str)
{
    Arg args[20];
    IswSetArg(args[0], IswNstring, str);
    IswSetValues(w, args, 1);
}

/* ---------- panel interface ---------- */

static void dm_apply(void)
{
    const char *tfmt = get_text(text_time_fmt);
    const char *dfmt = get_text(text_date_fmt);

    if (strcmp(tfmt, saved_time_fmt) != 0) {
        set_greeter_config("clock_time_format", tfmt);
        snprintf(saved_time_fmt, sizeof(saved_time_fmt), "%s", tfmt);
    }
    if (strcmp(dfmt, saved_date_fmt) != 0) {
        set_greeter_config("clock_date_format", dfmt);
        snprintf(saved_date_fmt, sizeof(saved_date_fmt), "%s", dfmt);
    }
}

static void dm_revert(void)
{
    set_text(text_time_fmt, saved_time_fmt);
    set_text(text_date_fmt, saved_date_fmt);
}

static Widget dm_create(Widget parent, IswAppContext app)
{
    (void)app;

    /* Fetch current values from the DM daemon */
    snprintf(saved_time_fmt, sizeof(saved_time_fmt), "%%H:%%M");
    snprintf(saved_date_fmt, sizeof(saved_date_fmt), "%%Y-%%m-%%d");
    fetch_greeter_config();

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgDefaultDistance(&ab, 8);
    IswArgBorderWidth(&ab, 0);
    Widget form = IswCreateWidget("dmPanel", formWidgetClass,
                                 parent, ab.args, ab.count);

    Dimension pw;
    Arg qa[20];
    IswSetArg(qa[0], IswNwidth, &pw);
    IswGetValues(parent, qa, 1);

    /* Time Format */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Time Format");
    IswArgWidth(&ab, LABEL_W);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgBorderWidth(&ab, 0);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    Widget time_lbl = IswCreateManagedWidget("timeFmtLabel",
                                            labelWidgetClass,
                                            form, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgFromHoriz(&ab, time_lbl);
    IswArgWidth(&ab, TEXT_W);
    IswArgEditType(&ab, IswtextEdit);
    IswArgBorderWidth(&ab, 1);
    IswArgString(&ab, saved_time_fmt);
    IswArgLeft(&ab, IswChainLeft);
    text_time_fmt = IswCreateManagedWidget("timeFmtText",
                                          asciiTextWidgetClass,
                                          form, ab.args, ab.count);

    /* Date Format */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Date Format");
    IswArgWidth(&ab, LABEL_W);
    IswArgJustify(&ab, IswJustifyRight);
    IswArgBorderWidth(&ab, 0);
    IswArgFromVert(&ab, time_lbl);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainLeft);
    Widget date_lbl = IswCreateManagedWidget("dateFmtLabel",
                                            labelWidgetClass,
                                            form, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgFromHoriz(&ab, date_lbl);
    IswArgFromVert(&ab, text_time_fmt);
    IswArgWidth(&ab, TEXT_W);
    IswArgEditType(&ab, IswtextEdit);
    IswArgBorderWidth(&ab, 1);
    IswArgString(&ab, saved_date_fmt);
    IswArgLeft(&ab, IswChainLeft);
    text_date_fmt = IswCreateManagedWidget("dateFmtText",
                                          asciiTextWidgetClass,
                                          form, ab.args, ab.count);

    return form;
}

static int dm_has_changes(void)
{
    if (!text_time_fmt) {
        return 0;
    }
    return strcmp(get_text(text_time_fmt), saved_time_fmt) != 0 ||
           strcmp(get_text(text_date_fmt), saved_date_fmt) != 0;
}

static void dm_destroy(void)
{
    text_time_fmt = NULL;
    text_date_fmt = NULL;
    if (sys_bus) {
        dbus_connection_unref(sys_bus);
        sys_bus = NULL;
    }
}

const IsdeSettingsPanel panel_dm = {
    .name        = "Display Manager",
    .icon        = NULL,
    .section     = "dm",
    .create      = dm_create,
    .apply       = dm_apply,
    .revert      = dm_revert,
    .has_changes = dm_has_changes,
    .destroy     = dm_destroy,
};
