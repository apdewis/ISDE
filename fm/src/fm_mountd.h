/*
 * fm_mountd.h -- D-Bus client for isde-mountd (org.isde.DiskManager)
 *
 * Provides mount/unmount/eject operations and device signal
 * subscriptions for the file manager's places sidebar.
 * All functions are no-ops when mountd is unavailable.
 */
#ifndef ISDE_FM_MOUNTD_H
#define ISDE_FM_MOUNTD_H

#include <dbus/dbus.h>

struct FmApp;

/* D-Bus constants */
#define FM_MOUNTD_SERVICE   "org.isde.DiskManager"
#define FM_MOUNTD_PATH      "/org/isde/DiskManager"
#define FM_MOUNTD_INTERFACE "org.isde.DiskManager"

/* Device info from ListDevices */
#define FM_DEV_PATH_LEN    256
#define FM_LABEL_LEN       256
#define FM_VENDOR_LEN      256
#define FM_FS_TYPE_LEN     64
#define FM_MOUNT_POINT_LEN 512
#define FM_MAX_DEVICES     64

typedef struct FmDeviceInfo {
    char    dev_path[FM_DEV_PATH_LEN];
    char    label[FM_LABEL_LEN];
    char    vendor[FM_VENDOR_LEN];
    char    fs_type[FM_FS_TYPE_LEN];
    char    mount_point[FM_MOUNT_POINT_LEN];
    int     is_mounted;
    int     is_ejectable;
} FmDeviceInfo;

/* Connect to the system bus and subscribe to mountd signals.
 * Returns 0 on success, -1 if mountd is not available. */
int  fm_mountd_init(struct FmApp *app);
void fm_mountd_cleanup(struct FmApp *app);

/* Call Mount/Unmount/Eject on a device.
 * result/result_len receives the mount point (Mount) or error string.
 * Returns 0 on success. */
int  fm_mountd_mount(struct FmApp *app, const char *dev_path,
                     char *result, size_t result_len);
int  fm_mountd_unmount(struct FmApp *app, const char *dev_path,
                       char *result, size_t result_len);
int  fm_mountd_eject(struct FmApp *app, const char *dev_path,
                     char *result, size_t result_len);

/* Look up a device by its label.
 * Returns pointer into the device array, or NULL. */
FmDeviceInfo *fm_mountd_find_by_label(struct FmApp *app, const char *label);

/* Look up a device by its mount point path.
 * Returns pointer into the device array, or NULL. */
FmDeviceInfo *fm_mountd_find_by_mount_point(struct FmApp *app, const char *path);

#endif /* ISDE_FM_MOUNTD_H */
