#define _POSIX_C_SOURCE 200809L
/*
 * password_dialog.c — passphrase entry dialog for LUKS devices
 *
 * Uses isde_dialog_input with echo disabled on the text sink,
 * matching the pattern from the network tray applet's agent.
 */
#include "tray-mount.h"

#include <ISW/IswArgMacros.h>
#include <ISW/Text.h>
#include <ISW/TextSink.h>

#include <stdio.h>
#include <string.h>

/* ---------- dialog callback ---------- */

static void on_input_result(IsdeDialogResult result, const char *text,
                            void *user_data)
{
    TrayMount *tm = (TrayMount *)user_data;

    if (result == ISDE_DIALOG_OK && text && text[0]) {
        char passphrase[256];
        snprintf(passphrase, sizeof(passphrase), "%s", text);
        tm_dbus_mount(tm, tm->pw_dev_path, passphrase);
        memset(passphrase, 0, sizeof(passphrase));
    }

    tm->pw_shell = NULL;
    memset(tm->pw_dev_path, 0, sizeof(tm->pw_dev_path));
}

/* ---------- public API ---------- */

void tm_password_dialog_show(TrayMount *tm, int device_idx)
{
    if (device_idx < 0 || device_idx >= tm->ndevices)
        return;

    tm_password_dialog_cleanup(tm);

    DeviceInfo *d = &tm->devices[device_idx];
    snprintf(tm->pw_dev_path, sizeof(tm->pw_dev_path), "%s", d->dev_path);

    tm->pw_shell = isde_dialog_input(
        tm->toplevel, "Unlock Encrypted Volume", "Enter passphrase", "",
        on_input_result, tm);

    Widget value_w = IswNameToWidget(tm->pw_shell, "*value");
    if (value_w) {
        IswArgBuilder ab = IswArgBuilderInit();
        IswArgBuilderAdd(&ab, IswNresize, (IswArgVal)IswtextResizeNever);
        IswArgWidth(&ab, 280);
        IswSetValues(value_w, ab.args, ab.count);

        Widget sink = NULL;
        IswArgBuilderReset(&ab);
        IswArgBuilderAdd(&ab, IswNtextSink, (IswArgVal)&sink);
        IswGetValues(value_w, ab.args, ab.count);
        if (sink) {
            IswArgBuilderReset(&ab);
            IswArgBuilderAdd(&ab, IswNecho, (IswArgVal)False);
            IswSetValues(sink, ab.args, ab.count);
        }
    }
}

void tm_password_dialog_cleanup(TrayMount *tm)
{
    if (tm->pw_shell) {
        isde_dialog_dismiss(tm->pw_shell);
        tm->pw_shell = NULL;
    }
    memset(tm->pw_dev_path, 0, sizeof(tm->pw_dev_path));
}
