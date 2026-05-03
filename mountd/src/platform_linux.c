#define _POSIX_C_SOURCE 200809L
/*
 * platform_linux.c — Linux device monitoring via libudev
 */
#include "mountd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/cdrom.h>
#include <libudev.h>

/* ---------- helpers ---------- */

static int is_removable(struct udev_device *dev)
{
    const char *id_bus = udev_device_get_property_value(dev, "ID_BUS");
    if (id_bus && strcmp(id_bus, "usb") == 0) {
        return 1;
    }

    /* Find the parent disk to check the removable sysattr.
     * udev_device_get_parent_with_subsystem_devtype skips the device
     * itself, so for a whole-disk device (no partitions) there is no
     * parent disk — check the device's own sysattr in that case. */
    struct udev_device *disk =
        udev_device_get_parent_with_subsystem_devtype(
            dev, "block", "disk");

    if (!disk) {
        const char *devtype = udev_device_get_devtype(dev);
        if (devtype && strcmp(devtype, "disk") == 0)
            disk = dev;
    }

    if (disk) {
        const char *removable = udev_device_get_sysattr_value(
            disk, "removable");
        if (removable && strcmp(removable, "1") == 0)
            return 1;
    }

    return 0;
}

static int is_ejectable(struct udev_device *dev)
{
    const char *id_bus = udev_device_get_property_value(dev, "ID_BUS");
    if (id_bus && strcmp(id_bus, "usb") == 0) {
        return 1;
    }

    /* Optical drives are ejectable */
    const char *id_cdrom = udev_device_get_property_value(dev, "ID_CDROM");
    if (id_cdrom) {
        return 1;
    }

    return 0;
}

static const char *get_label(struct udev_device *dev)
{
    const char *label = udev_device_get_property_value(dev, "ID_FS_LABEL");
    if (label && label[0]) {
        return label;
    }
    /* Fall back to partition label or device basename */
    label = udev_device_get_property_value(dev, "ID_PART_ENTRY_NAME");
    if (label && label[0]) {
        return label;
    }
    return NULL;
}

static const char *get_fs_type(struct udev_device *dev)
{
    return udev_device_get_property_value(dev, "ID_FS_TYPE");
}

static void get_vendor_model(struct udev_device *dev, char *buf, size_t len)
{
    const char *vendor = udev_device_get_property_value(dev, "ID_VENDOR");
    const char *model  = udev_device_get_property_value(dev, "ID_MODEL");

    if (vendor && vendor[0] && model && model[0]) {
        snprintf(buf, len, "%s %s", vendor, model);
    } else if (vendor && vendor[0]) {
        snprintf(buf, len, "%s", vendor);
    } else if (model && model[0]) {
        snprintf(buf, len, "%s", model);
    } else {
        buf[0] = '\0';
    }

    /* udev encodes spaces as underscores in these fields */
    for (char *p = buf; *p; p++) {
        if (*p == '_') *p = ' ';
    }
}

static void setup_luks_fields(Device *d, struct udev_device *dev,
                              const char *devnode)
{
    d->is_luks = 1;
    const char *uuid = udev_device_get_property_value(dev, "ID_FS_UUID");
    if (uuid && uuid[0]) {
        snprintf(d->luks_uuid, sizeof(d->luks_uuid), "%s", uuid);
        snprintf(d->dm_name, sizeof(d->dm_name), "luks-%s", uuid);
    } else {
        const char *base = strrchr(devnode, '/');
        base = base ? base + 1 : devnode;
        snprintf(d->dm_name, sizeof(d->dm_name), "luks-%s", base);
    }
    snprintf(d->dm_path, sizeof(d->dm_path), "/dev/mapper/%s", d->dm_name);
}

/* ---------- enumeration ---------- */

static void linux_enumerate_devices(MountDaemon *md)
{
    struct udev *udev = md->monitor_handle
        ? udev_monitor_get_udev((struct udev_monitor *)md->monitor_handle)
        : udev_new();
    if (!udev) {
        return;
    }

    struct udev_enumerate *en = udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(en, "block");
    udev_enumerate_scan_devices(en);

    struct udev_list_entry *entry;
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(en)) {
        const char *path = udev_list_entry_get_name(entry);
        struct udev_device *dev = udev_device_new_from_syspath(udev, path);
        if (!dev) {
            continue;
        }

        const char *devtype = udev_device_get_devtype(dev);
        int is_part = devtype && strcmp(devtype, "partition") == 0;
        int is_disk = devtype && strcmp(devtype, "disk") == 0;

        /* Accept partitions, and whole disks that have a filesystem
         * directly on them (e.g. LUKS on unpartitioned USB) */
        if (!is_part && !(is_disk && get_fs_type(dev))) {
            udev_device_unref(dev);
            continue;
        }

        if (is_removable(dev)) {
            const char *devnode = udev_device_get_devnode(dev);
            const char *label   = get_label(dev);
            const char *fs_type = get_fs_type(dev);
            char vendor[VENDOR_LEN];
            get_vendor_model(dev, vendor, sizeof(vendor));

            if (devnode) {
                Device *d = mountd_add_device(md, devnode,
                                              label, vendor, fs_type,
                                              is_ejectable(dev));
                if (d && fs_type && strcmp(fs_type, "crypto_LUKS") == 0)
                    setup_luks_fields(d, dev, devnode);
            }
        }

        udev_device_unref(dev);
    }

    udev_enumerate_unref(en);

    /* Don't unref udev here if it came from the monitor */
    if (!md->monitor_handle) {
        udev_unref(udev);
    }
}

/* ---------- monitor ---------- */

static int linux_monitor_init(MountDaemon *md)
{
    struct udev *udev = udev_new();
    if (!udev) {
        fprintf(stderr, "isde-mountd: udev_new() failed\n");
        return -1;
    }

    struct udev_monitor *mon = udev_monitor_new_from_netlink(udev, "udev");
    if (!mon) {
        fprintf(stderr, "isde-mountd: udev_monitor_new() failed\n");
        udev_unref(udev);
        return -1;
    }

    udev_monitor_filter_add_match_subsystem_devtype(mon, "block", "partition");
    udev_monitor_filter_add_match_subsystem_devtype(mon, "block", "disk");
    udev_monitor_enable_receiving(mon);

    md->monitor_fd = udev_monitor_get_fd(mon);
    md->monitor_handle = mon;

    fprintf(stderr, "isde-mountd: udev monitor active (fd=%d)\n",
            md->monitor_fd);
    return 0;
}

static void linux_monitor_dispatch(MountDaemon *md)
{
    struct udev_monitor *mon = (struct udev_monitor *)md->monitor_handle;
    struct udev_device *dev = udev_monitor_receive_device(mon);
    if (!dev) {
        return;
    }

    const char *action  = udev_device_get_action(dev);
    const char *devnode = udev_device_get_devnode(dev);
    const char *devtype = udev_device_get_devtype(dev);

    if (!action || !devnode) {
        udev_device_unref(dev);
        return;
    }

    int is_part = devtype && strcmp(devtype, "partition") == 0;
    int is_disk = devtype && strcmp(devtype, "disk") == 0;

    /* Accept partitions, and whole disks that carry a filesystem
     * directly (e.g. LUKS on unpartitioned USB) */
    if (!is_part && !(is_disk && get_fs_type(dev))) {
        udev_device_unref(dev);
        return;
    }

    fprintf(stderr, "isde-mountd: udev: %s %s\n", action, devnode);

    if (strcmp(action, "add") == 0) {
        if (is_removable(dev)) {
            const char *label   = get_label(dev);
            const char *fs_type = get_fs_type(dev);
            char vendor[VENDOR_LEN];
            get_vendor_model(dev, vendor, sizeof(vendor));

            Device *d = mountd_add_device(md, devnode, label, vendor,
                                          fs_type, is_ejectable(dev));
            if (d) {
                if (fs_type && strcmp(fs_type, "crypto_LUKS") == 0)
                    setup_luks_fields(d, dev, devnode);

                mountd_dbus_emit_device_added(md, d);

                if (md->auto_mount && !d->is_luks) {
                    char mp[MOUNT_POINT_LEN], err[256];
                    mountd_do_mount(md, devnode, 0, "",
                                    mp, sizeof(mp), err, sizeof(err));
                }
            }
        }
    } else if (strcmp(action, "remove") == 0) {
        Device *d = mountd_find_device(md, devnode);
        if (d) {
            /* If still mounted, the kernel already removed it;
             * just update our state */
            d->is_mounted = 0;
            d->mount_point[0] = '\0';
            mountd_dbus_emit_device_removed(md, devnode);
            mountd_remove_device(md, devnode);
        }
    } else if (strcmp(action, "change") == 0) {
        /* Media change (e.g. optical disc inserted) — re-check */
        if (is_removable(dev)) {
            Device *d = mountd_find_device(md, devnode);
            if (!d) {
                const char *label   = get_label(dev);
                const char *fs_type = get_fs_type(dev);
                char vendor[VENDOR_LEN];
                get_vendor_model(dev, vendor, sizeof(vendor));
                d = mountd_add_device(md, devnode, label, vendor,
                                      fs_type, is_ejectable(dev));
                if (d) {
                    if (fs_type &&
                        strcmp(fs_type, "crypto_LUKS") == 0)
                        setup_luks_fields(d, dev, devnode);
                    mountd_dbus_emit_device_added(md, d);
                }
            }
        }
    }

    udev_device_unref(dev);
}

static void linux_monitor_cleanup(MountDaemon *md)
{
    if (md->monitor_handle) {
        struct udev_monitor *mon = (struct udev_monitor *)md->monitor_handle;
        struct udev *udev = udev_monitor_get_udev(mon);
        udev_monitor_unref(mon);
        udev_unref(udev);
        md->monitor_handle = NULL;
        md->monitor_fd = -1;
    }
}

/* ---------- eject ---------- */

static int linux_eject(const char *dev_path)
{
    /* Try CDROMEJECT ioctl first (for optical drives) */
    int fd = open(dev_path, O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
        if (ioctl(fd, CDROMEJECT, 0) == 0) {
            close(fd);
            return 0;
        }
        close(fd);
    }

    /* For USB devices, find the parent device and write to
     * /sys/block/<disk>/device/delete to safely remove */
    char sysfs_path[512];

    /* Extract disk name from device path: /dev/sdb1 -> sdb */
    const char *basename = strrchr(dev_path, '/');
    if (!basename) {
        return -1;
    }
    basename++;

    /* Strip partition number to get disk name */
    char disk[64];
    snprintf(disk, sizeof(disk), "%s", basename);
    size_t len = strlen(disk);
    while (len > 0 && disk[len - 1] >= '0' && disk[len - 1] <= '9') {
        disk[--len] = '\0';
    }

    snprintf(sysfs_path, sizeof(sysfs_path),
             "/sys/block/%s/device/delete", disk);

    fd = open(sysfs_path, O_WRONLY);
    if (fd >= 0) {
        write(fd, "1", 1);
        close(fd);
        return 0;
    }

    return -1;
}

/* ---------- ops table ---------- */

static const MountdPlatformOps linux_ops = {
    .monitor_init      = linux_monitor_init,
    .monitor_dispatch  = linux_monitor_dispatch,
    .monitor_cleanup   = linux_monitor_cleanup,
    .enumerate_devices = linux_enumerate_devices,
    .eject             = linux_eject,
};

const MountdPlatformOps *mountd_platform_ops(void)
{
    return &linux_ops;
}
