/*
 * tray-mount.h — isde-tray-mount internal header
 */
#ifndef ISDE_TRAY_MOUNT_H
#define ISDE_TRAY_MOUNT_H

#include <ISW/Intrinsic.h>
#include <ISW/StringDefs.h>
#include <ISW/Shell.h>
#include <ISW/SimpleMenu.h>
#include <ISW/SmeBSB.h>
#include <ISW/SmeLine.h>
#include <ISW/IswTrayIcon.h>
#include <ISW/ISWSVG.h>

#include <xcb/xcb.h>

#include <dbus/dbus.h>

#include "isde/isde-dbus.h"
#include "isde/isde-theme.h"

/* ---------- D-Bus constants ---------- */

#define MOUNTD_DBUS_SERVICE   "org.isde.DiskManager"
#define MOUNTD_DBUS_PATH      "/org/isde/DiskManager"
#define MOUNTD_DBUS_INTERFACE "org.isde.DiskManager"

/* ---------- Device info ---------- */

#define DEV_PATH_LEN    256
#define LABEL_LEN       256
#define FS_TYPE_LEN     64
#define MOUNT_POINT_LEN 512
#define MAX_DEVICES     64

typedef struct DeviceInfo {
    char    dev_path[DEV_PATH_LEN];
    char    label[LABEL_LEN];
    char    fs_type[FS_TYPE_LEN];
    char    mount_point[MOUNT_POINT_LEN];
    int     is_mounted;
    int     is_ejectable;
} DeviceInfo;

/* ---------- Tray applet state ---------- */

typedef struct TrayMount {
    IswAppContext       app;
    Widget              toplevel;

    /* Tray icon (raw XCB window, managed by ISW) */
    IswTrayIcon         tray_icon;

    /* Popup menu */
    Widget              menu_shell;

    /* Device state */
    DeviceInfo          devices[MAX_DEVICES];
    int                 ndevices;

    /* D-Bus */
    DBusConnection     *system_bus;
    IsdeDBus           *session_dbus;

    int                 running;
    int                 restart;
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
                   char *result, size_t result_len);
int  tm_dbus_unmount(TrayMount *tm, const char *dev_path,
                     char *errbuf, size_t errlen);
int  tm_dbus_eject(TrayMount *tm, const char *dev_path,
                   char *errbuf, size_t errlen);

/* ---------- menu.c ---------- */
void tm_menu_init(TrayMount *tm);
void tm_menu_show(TrayMount *tm);
void tm_menu_cleanup(TrayMount *tm);

#endif /* ISDE_TRAY_MOUNT_H */
