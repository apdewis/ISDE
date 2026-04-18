/*
 * mountd.h — isde-mountd internal header
 */
#ifndef ISDE_MOUNTD_H
#define ISDE_MOUNTD_H

#include "platform.h"

#include <sys/types.h>
#include <signal.h>
#include <dbus/dbus.h>

/* ---------- D-Bus constants ---------- */

#define MOUNTD_DBUS_SERVICE   "org.isde.DiskManager"
#define MOUNTD_DBUS_PATH      "/org/isde/DiskManager"
#define MOUNTD_DBUS_INTERFACE "org.isde.DiskManager"

/* ---------- Limits ---------- */

#define MAX_DEVICES     64
#define DEV_PATH_LEN    256
#define LABEL_LEN       256
#define VENDOR_LEN      256
#define FS_TYPE_LEN     64
#define MOUNT_POINT_LEN 512

/* ---------- Allowed filesystem types ---------- */

static const char *const allowed_fs_types[] = {
    "vfat", "exfat", "ext2", "ext3", "ext4",
    "ntfs", "ntfs3", "iso9660", "udf", NULL
};

/* ---------- Device tracking ---------- */

typedef struct Device {
    char    dev_path[DEV_PATH_LEN];
    char    label[LABEL_LEN];
    char    vendor[VENDOR_LEN];
    char    fs_type[FS_TYPE_LEN];
    char    mount_point[MOUNT_POINT_LEN];
    int     is_mounted;
    int     is_ejectable;
} Device;

/* ---------- Daemon state ---------- */

typedef struct MountDaemon {
    /* Device list */
    Device  devices[MAX_DEVICES];
    int     ndevices;

    /* Platform device monitor */
    const MountdPlatformOps *plat;
    int     monitor_fd;
    void   *monitor_handle;

    /* D-Bus (system bus) */
    DBusConnection *dbus;

    /* Config */
    char   *mount_base;        /* /media */
    int     auto_mount;

    /* Signal self-pipe */
    int     sig_pipe[2];

    /* Control */
    volatile sig_atomic_t running;
} MountDaemon;

/* ---------- mountd.c ---------- */
int  mountd_init(MountDaemon *md);
void mountd_run(MountDaemon *md);
void mountd_cleanup(MountDaemon *md);

/* ---------- dbus.c ---------- */
int  mountd_dbus_init(MountDaemon *md);
void mountd_dbus_cleanup(MountDaemon *md);
void mountd_dbus_dispatch(MountDaemon *md);
int  mountd_dbus_get_fd(MountDaemon *md);
void mountd_dbus_emit_device_added(MountDaemon *md, const Device *dev);
void mountd_dbus_emit_device_removed(MountDaemon *md, const char *dev_path);
void mountd_dbus_emit_device_mounted(MountDaemon *md, const char *dev_path,
                                     const char *mount_point);
void mountd_dbus_emit_device_unmounted(MountDaemon *md, const char *dev_path);

/* ---------- devices.c ---------- */
Device *mountd_find_device(MountDaemon *md, const char *dev_path);
Device *mountd_add_device(MountDaemon *md, const char *dev_path,
                          const char *label, const char *vendor,
                          const char *fs_type, int ejectable);
void    mountd_remove_device(MountDaemon *md, const char *dev_path);
void    mountd_refresh_mount_state(MountDaemon *md);

/* ---------- mount_ops.c ---------- */
int  mountd_do_mount(MountDaemon *md, const char *dev_path,
                     unsigned long caller_uid,
                     char *mount_point_out, size_t mp_len,
                     char *errbuf, size_t errlen);
int  mountd_do_unmount(MountDaemon *md, const char *dev_path,
                       char *errbuf, size_t errlen);
int  mountd_do_eject(MountDaemon *md, const char *dev_path,
                     char *errbuf, size_t errlen);

#endif /* ISDE_MOUNTD_H */
