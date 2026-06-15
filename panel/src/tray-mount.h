/*
 * tray-mount.h — removable disk tray module for isde-panel
 */
#ifndef ISDE_TRAY_MOUNT_H
#define ISDE_TRAY_MOUNT_H

#include "panel.h"

#include <dbus/dbus.h>

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
#define TM_MAX_DEVICES     64

typedef struct MountDeviceInfo {
    char    dev_path[DEV_PATH_LEN];
    char    label[LABEL_LEN];
    char    vendor[VENDOR_LEN];
    char    fs_type[FS_TYPE_LEN];
    char    mount_point[MOUNT_POINT_LEN];
    int     is_mounted;
    int     is_ejectable;
    int     is_luks;
} MountDeviceInfo;

/* ---------- Tray module state ---------- */

typedef struct TrayMount {
    struct Panel   *panel;

    Widget          icon;

    /* Popup */
    Widget          popup_shell;
    Widget          popup_outer;
    Widget          popup_viewport;
    Widget          popup_listbox;
    int             popup_visible;

    /* Device state */
    MountDeviceInfo      devices[TM_MAX_DEVICES];
    int             ndevices;

    /* D-Bus (system bus — isde-mountd lives here) */
    DBusConnection *system_bus;

    /* Password dialog */
    Widget          pw_shell;
    char            pw_dev_path[DEV_PATH_LEN];
} TrayMount;

/* ---------- tray-mount.c ---------- */
void tn_mount_init(struct Panel *p);
void tn_mount_cleanup(struct Panel *p);
void tn_mount_reload_theme(TrayMount *tm);

/* ---------- tray-mount-dbus.c ---------- */
int  tm_dbus_init(TrayMount *tm);
void tm_dbus_cleanup(TrayMount *tm);
int  tm_dbus_list_devices(TrayMount *tm);
void tm_dbus_mount(TrayMount *tm, const char *dev_path,
                   const char *passphrase);
void tm_dbus_unmount(TrayMount *tm, const char *dev_path);
void tm_dbus_eject(TrayMount *tm, const char *dev_path);

/* ---------- tray-mount-popup.c ---------- */
void tm_popup_init(TrayMount *tm);
void tm_popup_show(TrayMount *tm);
void tm_popup_refresh(TrayMount *tm);
void tm_popup_hide(TrayMount *tm);
void tm_popup_cleanup(TrayMount *tm);

/* ---------- tray-mount-password.c ---------- */
void tm_password_dialog_show(TrayMount *tm, int device_idx);
void tm_password_dialog_cleanup(TrayMount *tm);

#endif /* ISDE_TRAY_MOUNT_H */
