/*
 * tray-mount.h — isde-tray-mount internal header
 */
#ifndef ISDE_TRAY_MOUNT_H
#define ISDE_TRAY_MOUNT_H

#include <ISW/Intrinsic.h>
#include <ISW/StringDefs.h>
#include <ISW/Shell.h>
#include <ISW/Command.h>
#include <ISW/Label.h>
#include <ISW/FlexBox.h>
#include <ISW/Viewport.h>
#include <ISW/ListBox.h>
#include <ISW/ListBoxRow.h>
#include <ISW/IswTrayIcon.h>
#include <ISW/ISWSVG.h>

#include <xcb/xcb.h>

#include <dbus/dbus.h>

#include "isde/isde-dbus.h"
#include "isde/isde-dialog.h"
#include "isde/isde-theme.h"
#include "isde/isde-xdg.h"

/* ---------- D-Bus constants ---------- */

#define MOUNTD_DBUS_SERVICE   "org.isde.DiskManager"
#define MOUNTD_DBUS_PATH      "/org/isde/DiskManager"
#define MOUNTD_DBUS_INTERFACE "org.isde.DiskManager"

/* ---------- Device info ---------- */

#define DEV_PATH_LEN    256
#define LABEL_LEN       256
#define VENDOR_LEN      256
#define FS_TYPE_LEN     64
#define MOUNT_POINT_LEN 512
#define MAX_DEVICES     64

typedef struct DeviceInfo {
    char    dev_path[DEV_PATH_LEN];
    char    label[LABEL_LEN];
    char    vendor[VENDOR_LEN];
    char    fs_type[FS_TYPE_LEN];
    char    mount_point[MOUNT_POINT_LEN];
    int     is_mounted;
    int     is_ejectable;
    int     is_luks;
} DeviceInfo;

/* ---------- Tray applet state ---------- */

typedef struct TrayMount {
    IswAppContext       app;
    Widget              toplevel;

    /* Tray icon (raw XCB window, managed by ISW) */
    IswTrayIcon         tray_icon;

    /* Popup */
    Widget              popup_shell;
    Widget              popup_outer;
    Widget              popup_viewport;
    int                 popup_visible;

    /* Device state */
    DeviceInfo          devices[MAX_DEVICES];
    int                 ndevices;

    /* D-Bus */
    DBusConnection     *system_bus;
    IsdeDBus           *session_dbus;

    int                 running;
    int                 restart;

    /* Password dialog */
    Widget              pw_shell;
    char                pw_dev_path[DEV_PATH_LEN];
} TrayMount;

/* ---------- tray-mount.c ---------- */
int  tray_mount_init(TrayMount *tm, int *argc, char **argv);
void tray_mount_run(TrayMount *tm);
void tray_mount_cleanup(TrayMount *tm);

/* ---------- dbus.c ---------- */
int  tm_dbus_init(TrayMount *tm);
void tm_dbus_cleanup(TrayMount *tm);
int  tm_dbus_list_devices(TrayMount *tm);
int  tm_dbus_mount(TrayMount *tm, const char *dev_path,
                   const char *passphrase,
                   char *result, size_t result_len);
int  tm_dbus_unmount(TrayMount *tm, const char *dev_path,
                     char *errbuf, size_t errlen);
int  tm_dbus_eject(TrayMount *tm, const char *dev_path,
                   char *errbuf, size_t errlen);

/* ---------- popup.c ---------- */
void tm_popup_init(TrayMount *tm);
void tm_popup_show(TrayMount *tm);
void tm_popup_hide(TrayMount *tm);
void tm_popup_cleanup(TrayMount *tm);

/* ---------- password_dialog.c ---------- */
void tm_password_dialog_show(TrayMount *tm, int device_idx);
void tm_password_dialog_cleanup(TrayMount *tm);

#endif /* ISDE_TRAY_MOUNT_H */
