#define _POSIX_C_SOURCE 200809L
/*
 * mount_ops.c — mount/unmount/eject operations
 */
#include "mountd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <pwd.h>

/* ---------- helpers ---------- */

static int is_fs_type_allowed(const char *fs)
{
    for (int i = 0; allowed_fs_types[i]; i++) {
        if (strcmp(fs, allowed_fs_types[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Sanitize label for use as directory name: replace / and leading . */
static void sanitize_label(const char *label, char *out, size_t len)
{
    size_t j = 0;
    for (size_t i = 0; label[i] && j < len - 1; i++) {
        char c = label[i];
        if (c == '/' || c == '\0') {
            out[j++] = '_';
        } else if (i == 0 && c == '.') {
            out[j++] = '_';
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
    if (j == 0) {
        snprintf(out, len, "disk");
    }
}

static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) < 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    return mkdir(tmp, mode) < 0 && errno != EEXIST ? -1 : 0;
}

/* ---------- mount ---------- */

int mountd_do_mount(MountDaemon *md, const char *dev_path,
                    unsigned long caller_uid, const char *passphrase,
                    char *mount_point_out, size_t mp_len,
                    char *errbuf, size_t errlen)
{
    Device *dev = mountd_find_device(md, dev_path);
    if (!dev) {
        snprintf(errbuf, errlen, "Device not tracked: %s", dev_path);
        return -1;
    }

    if (dev->is_mounted) {
        snprintf(errbuf, errlen, "Already mounted at %s", dev->mount_point);
        return -1;
    }

    /* LUKS: unlock the container first */
    const char *mount_dev = dev_path;
    const char *fs = dev->fs_type;

    if (dev->is_luks) {
        if (!passphrase || !passphrase[0]) {
            snprintf(errbuf, errlen, "Passphrase required for LUKS device");
            return -1;
        }

        if (!dev->dm_name[0]) {
            snprintf(errbuf, errlen, "LUKS device has no dm name");
            return -1;
        }

        if (!dev->is_unlocked) {
            if (mountd_luks_open(dev_path, dev->dm_name,
                                 passphrase, strlen(passphrase),
                                 errbuf, errlen) != 0) {
                return -1;
            }
            dev->is_unlocked = 1;
            dev->luks_opened_by_us = 1;
        }

        /* Probe inner filesystem */
        if (!dev->inner_fs_type[0]) {
            if (mountd_luks_probe_fs(dev->dm_path,
                                     dev->inner_fs_type,
                                     sizeof(dev->inner_fs_type)) != 0) {
                snprintf(errbuf, errlen,
                         "Cannot determine filesystem inside LUKS container");
                if (dev->luks_opened_by_us) {
                    char closeerr[128];
                    mountd_luks_close(dev->dm_name, closeerr,
                                     sizeof(closeerr));
                    dev->is_unlocked = 0;
                    dev->luks_opened_by_us = 0;
                }
                return -1;
            }
        }

        mount_dev = dev->dm_path;
        fs = dev->inner_fs_type;
    }

    if (!fs[0]) {
        snprintf(errbuf, errlen, "Unknown filesystem type");
        return -1;
    }

    if (!is_fs_type_allowed(fs)) {
        snprintf(errbuf, errlen, "Filesystem type not allowed: %s", fs);
        if (dev->is_luks && dev->luks_opened_by_us) {
            char closeerr[128];
            mountd_luks_close(dev->dm_name, closeerr, sizeof(closeerr));
            dev->is_unlocked = 0;
            dev->luks_opened_by_us = 0;
        }
        return -1;
    }

    /* Resolve caller username */
    struct passwd *pw = getpwuid(caller_uid);
    if (!pw) {
        snprintf(errbuf, errlen, "Cannot resolve UID %lu", caller_uid);
        return -1;
    }

    /* Build mount point: /media/<user>/<label> */
    char safe_label[256];
    sanitize_label(dev->label, safe_label, sizeof(safe_label));

    char mp[MOUNT_POINT_LEN];
    snprintf(mp, sizeof(mp), "%s/%s/%s", md->mount_base, pw->pw_name,
             safe_label);

    /* Create mount point directory */
    if (mkdir_p(mp, 0755) != 0) {
        snprintf(errbuf, errlen, "Cannot create %s: %s", mp,
                 strerror(errno));
        return -1;
    }

    /* Determine mount options based on filesystem type */
    char opts[256];
    int is_fat = (strcmp(fs, "vfat") == 0 || strcmp(fs, "exfat") == 0);

    if (is_fat) {
        snprintf(opts, sizeof(opts), "uid=%lu,gid=%lu",
                 caller_uid, (unsigned long)pw->pw_gid);
    } else {
        opts[0] = '\0';
    }

    /* Mount */
    unsigned long flags = MS_NOSUID | MS_NODEV | MS_RELATIME;
    if (is_fat) {
        flags |= MS_NOEXEC;
    }

    if (mount(mount_dev, mp, fs, flags, opts) != 0) {
        snprintf(errbuf, errlen, "mount failed: %s", strerror(errno));
        rmdir(mp);
        if (dev->is_luks && dev->luks_opened_by_us) {
            char closeerr[128];
            mountd_luks_close(dev->dm_name, closeerr, sizeof(closeerr));
            dev->is_unlocked = 0;
            dev->luks_opened_by_us = 0;
        }
        return -1;
    }

    /* Update device state */
    dev->is_mounted = 1;
    snprintf(dev->mount_point, sizeof(dev->mount_point), "%s", mp);
    snprintf(mount_point_out, mp_len, "%s", mp);

    fprintf(stderr, "isde-mountd: mounted %s at %s\n", mount_dev, mp);
    return 0;
}

/* ---------- unmount ---------- */

int mountd_do_unmount(MountDaemon *md, const char *dev_path,
                      char *errbuf, size_t errlen)
{
    Device *dev = mountd_find_device(md, dev_path);
    if (!dev) {
        snprintf(errbuf, errlen, "Device not tracked: %s", dev_path);
        return -1;
    }

    if (!dev->is_mounted) {
        snprintf(errbuf, errlen, "Not mounted");
        return -1;
    }

    if (umount(dev->mount_point) != 0) {
        snprintf(errbuf, errlen, "umount failed: %s", strerror(errno));
        return -1;
    }

    /* Remove mount point directory (best effort) */
    rmdir(dev->mount_point);

    fprintf(stderr, "isde-mountd: unmounted %s from %s\n",
            dev_path, dev->mount_point);

    dev->is_mounted = 0;
    dev->mount_point[0] = '\0';

    /* Close LUKS container if we opened it */
    if (dev->is_luks && dev->is_unlocked && dev->luks_opened_by_us) {
        char closeerr[256];
        if (mountd_luks_close(dev->dm_name, closeerr,
                              sizeof(closeerr)) == 0) {
            dev->is_unlocked = 0;
            dev->luks_opened_by_us = 0;
            dev->inner_fs_type[0] = '\0';
        } else {
            fprintf(stderr, "isde-mountd: LUKS close failed: %s\n",
                    closeerr);
        }
    }

    return 0;
}

/* ---------- eject ---------- */

int mountd_do_eject(MountDaemon *md, const char *dev_path,
                    char *errbuf, size_t errlen)
{
    Device *dev = mountd_find_device(md, dev_path);
    if (!dev) {
        snprintf(errbuf, errlen, "Device not tracked: %s", dev_path);
        return -1;
    }

    /* Unmount first if mounted */
    if (dev->is_mounted) {
        if (mountd_do_unmount(md, dev_path, errbuf, errlen) != 0) {
            return -1;
        }
    }

    /* Platform-specific eject */
    if (md->plat->eject(dev_path) != 0) {
        snprintf(errbuf, errlen, "Eject failed for %s", dev_path);
        return -1;
    }

    /* Remove from tracking — the udev remove event will also fire,
     * but mountd_remove_device is idempotent. */
    mountd_remove_device(md, dev_path);

    fprintf(stderr, "isde-mountd: ejected %s\n", dev_path);
    return 0;
}
