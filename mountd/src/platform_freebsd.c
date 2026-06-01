/*
 * platform_freebsd.c — FreeBSD device monitoring via devd + GEOM
 *
 * Hotplug events come from the devd socket; the existing device set is
 * discovered by walking the GEOM tree (libgeom).  Filesystem type is
 * determined by probing the on-disk superblock, and labels come from the
 * GEOM LABEL class.  Eject uses CDIOCEJECT for optical media and a SCSI
 * START STOP UNIT (eject) for USB mass storage.
 */
#include "mountd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/cdio.h>
#include <libgeom.h>
#include <camlib.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_message.h>

/* ---------- name helpers ---------- */

static int is_da(const char *name)
{
    return strncmp(name, "da", 2) == 0 && isdigit((unsigned char)name[2]);
}

static int is_cd(const char *name)
{
    return strncmp(name, "cd", 2) == 0 && isdigit((unsigned char)name[2]);
}

/* Treat USB mass storage (da*) and optical (cd*) as the removable set —
 * FreeBSD exposes no clean "removable" attribute, mirroring the Linux
 * "USB == removable" heuristic. */
static int is_removable_name(const char *name)
{
    return is_da(name) || is_cd(name);
}

/* Reduce a provider name (da0p1, cd0) to its parent disk (da0, cd0). */
static void disk_of(const char *prov, char *out, size_t len)
{
    size_t i = 0;
    while (prov[i] && isalpha((unsigned char)prov[i]) && i < len - 1) {
        out[i] = prov[i];
        i++;
    }
    while (prov[i] && isdigit((unsigned char)prov[i]) && i < len - 1) {
        out[i] = prov[i];
        i++;
    }
    out[i] = '\0';
}

/* ---------- superblock probing ---------- */

/* Read the superblock of devnode and classify the filesystem, returning a
 * Linux-style name so Device.fs_type matches the shared allowed_fs_types
 * vocabulary.  Returns 0 and fills out on success, -1 otherwise. */
static int probe_fs_type(const char *devnode, char *out, size_t len)
{
    out[0] = '\0';

    int fd = open(devnode, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    enum { BUFSZ = 0x9800 };
    unsigned char *buf = calloc(1, BUFSZ);
    if (!buf) {
        close(fd);
        return -1;
    }

    ssize_t got = pread(fd, buf, BUFSZ, 0);
    close(fd);
    if (got < 1024) {
        free(buf);
        return -1;
    }

    int rv = 0;

    if (got >= 6 && memcmp(buf, "LUKS\xba\xbe", 6) == 0) {
        snprintf(out, len, "crypto_LUKS");
    } else if (got >= 11 && memcmp(buf + 3, "NTFS    ", 8) == 0) {
        snprintf(out, len, "ntfs");
    } else if (got >= 11 && memcmp(buf + 3, "EXFAT   ", 8) == 0) {
        snprintf(out, len, "exfat");
    } else if (got >= 1124 && buf[1080] == 0x53 && buf[1081] == 0xEF) {
        /* ext2/3/4: magic 0xEF53 at offset 1080 (little-endian) */
        uint32_t compat = buf[1116] | buf[1117] << 8 |
                          buf[1118] << 16 | (uint32_t)buf[1119] << 24;
        uint32_t incompat = buf[1120] | buf[1121] << 8 |
                            buf[1122] << 16 | (uint32_t)buf[1123] << 24;
        if (incompat & 0xC0) {          /* extents | 64bit */
            snprintf(out, len, "ext4");
        } else if (compat & 0x4) {      /* has_journal */
            snprintf(out, len, "ext3");
        } else {
            snprintf(out, len, "ext2");
        }
    } else if (got >= 512 && buf[510] == 0x55 && buf[511] == 0xAA &&
               (memcmp(buf + 0x36, "FAT", 3) == 0 ||
                memcmp(buf + 0x52, "FAT32", 5) == 0)) {
        snprintf(out, len, "vfat");
    } else if (got > 0x8006 && memcmp(buf + 0x8001, "CD001", 5) == 0) {
        /* ISO9660 PVD present; check for a following UDF NSR descriptor */
        int udf = 0;
        for (off_t o = 0x8001; o + 5 < got; o += 0x800) {
            if (memcmp(buf + o, "NSR0", 4) == 0) {
                udf = 1;
                break;
            }
        }
        snprintf(out, len, udf ? "udf" : "iso9660");
    } else {
        rv = -1;
    }

    free(buf);
    return rv;
}

/* ---------- GEOM lookups ---------- */

static int label_from_mesh(struct gmesh *mesh, const char *prov,
                           char *out, size_t len)
{
    struct gclass *cls;
    struct ggeom *gp;

    LIST_FOREACH(cls, &mesh->lg_class, lg_class) {
        if (strcmp(cls->lg_name, "LABEL") != 0) {
            continue;
        }
        LIST_FOREACH(gp, &cls->lg_geom, lg_geom) {
            struct gconsumer *cp = LIST_FIRST(&gp->lg_consumer);
            if (!cp || !cp->lg_provider) {
                continue;
            }
            if (strcmp(cp->lg_provider->lg_name, prov) != 0) {
                continue;
            }
            struct gprovider *pp = LIST_FIRST(&gp->lg_provider);
            if (!pp) {
                continue;
            }
            const char *b = strrchr(pp->lg_name, '/');
            b = b ? b + 1 : pp->lg_name;
            snprintf(out, len, "%s", b);
            return 0;
        }
    }
    out[0] = '\0';
    return -1;
}

static void disk_descr(struct gmesh *mesh, const char *disk,
                       char *out, size_t len)
{
    struct gclass *cls;
    struct ggeom *gp;
    struct gprovider *pp;
    struct gconfig *cf;

    out[0] = '\0';
    LIST_FOREACH(cls, &mesh->lg_class, lg_class) {
        if (strcmp(cls->lg_name, "DISK") != 0) {
            continue;
        }
        LIST_FOREACH(gp, &cls->lg_geom, lg_geom) {
            if (strcmp(gp->lg_name, disk) != 0) {
                continue;
            }
            /* "descr" lives in the provider's config, not the geom's */
            LIST_FOREACH(pp, &gp->lg_provider, lg_provider) {
                LIST_FOREACH(cf, &pp->lg_config, lg_config) {
                    if (strcmp(cf->lg_name, "descr") == 0 && cf->lg_val) {
                        snprintf(out, len, "%s", cf->lg_val);
                        return;
                    }
                }
            }
        }
    }
}

static int has_part_geom(struct gmesh *mesh, const char *disk)
{
    struct gclass *cls;
    struct ggeom *gp;

    LIST_FOREACH(cls, &mesh->lg_class, lg_class) {
        if (strcmp(cls->lg_name, "PART") != 0) {
            continue;
        }
        LIST_FOREACH(gp, &cls->lg_geom, lg_geom) {
            if (strcmp(gp->lg_name, disk) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

/* ---------- device add ---------- */

static void set_luks_fields(Device *d, const char *devnode)
{
    d->is_luks = 1;
    const char *base = strrchr(devnode, '/');
    base = base ? base + 1 : devnode;
    snprintf(d->dm_name, sizeof(d->dm_name), "luks-%s", base);
    snprintf(d->dm_path, sizeof(d->dm_path), "/dev/mapper/%s", d->dm_name);
}

static void add_candidate(MountDaemon *md, struct gmesh *mesh,
                          const char *prov, const char *disk,
                          int require_fs, int is_event)
{
    char devnode[DEV_PATH_LEN];
    snprintf(devnode, sizeof(devnode), "/dev/%s", prov);

    char fs[FS_TYPE_LEN];
    probe_fs_type(devnode, fs, sizeof(fs));
    if (require_fs && !fs[0]) {
        return;
    }

    char label[LABEL_LEN];
    label_from_mesh(mesh, prov, label, sizeof(label));

    char vendor[VENDOR_LEN];
    disk_descr(mesh, disk, vendor, sizeof(vendor));

    Device *d = mountd_add_device(md, devnode,
                                  label[0] ? label : NULL,
                                  vendor[0] ? vendor : NULL,
                                  fs[0] ? fs : NULL,
                                  is_removable_name(disk));
    if (!d) {
        return;
    }
    if (fs[0] && strcmp(fs, "crypto_LUKS") == 0) {
        set_luks_fields(d, devnode);
    }

    if (is_event) {
        mountd_dbus_emit_device_added(md, d);
        if (md->auto_mount && !d->is_luks) {
            char mp[MOUNT_POINT_LEN], err[256];
            mountd_do_mount(md, devnode, 0, "",
                            mp, sizeof(mp), err, sizeof(err));
        }
    }
}

/* ---------- enumeration ---------- */

static void freebsd_enumerate_devices(MountDaemon *md)
{
    struct gmesh mesh;
    if (geom_gettree(&mesh) != 0) {
        return;
    }

    struct gclass *cls;
    struct ggeom *gp;
    struct gprovider *pp;

    /* Partitions on removable disks */
    LIST_FOREACH(cls, &mesh.lg_class, lg_class) {
        if (strcmp(cls->lg_name, "PART") != 0) {
            continue;
        }
        LIST_FOREACH(gp, &cls->lg_geom, lg_geom) {
            if (!is_removable_name(gp->lg_name)) {
                continue;
            }
            LIST_FOREACH(pp, &gp->lg_provider, lg_provider) {
                add_candidate(md, &mesh, pp->lg_name, gp->lg_name, 0, 0);
            }
        }
    }

    /* Whole-disk filesystems on removable disks with no partition table */
    LIST_FOREACH(cls, &mesh.lg_class, lg_class) {
        if (strcmp(cls->lg_name, "DISK") != 0) {
            continue;
        }
        LIST_FOREACH(gp, &cls->lg_geom, lg_geom) {
            if (!is_removable_name(gp->lg_name)) {
                continue;
            }
            if (has_part_geom(&mesh, gp->lg_name)) {
                continue;
            }
            LIST_FOREACH(pp, &gp->lg_provider, lg_provider) {
                add_candidate(md, &mesh, pp->lg_name, gp->lg_name, 1, 0);
            }
        }
    }

    geom_deletetree(&mesh);
}

/* ---------- devd monitor ---------- */

static int freebsd_monitor_init(MountDaemon *md)
{
    struct sockaddr_un sa;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);

    if (fd >= 0) {
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        snprintf(sa.sun_path, sizeof(sa.sun_path),
                 "/var/run/devd.seqpacket.pipe");
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            close(fd);
            fd = -1;
        }
    }

    if (fd < 0) {
        /* Legacy stream pipe */
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            memset(&sa, 0, sizeof(sa));
            sa.sun_family = AF_UNIX;
            snprintf(sa.sun_path, sizeof(sa.sun_path), "/var/run/devd.pipe");
            if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
                close(fd);
                fd = -1;
            }
        }
    }

    if (fd < 0) {
        fprintf(stderr, "isde-mountd: cannot connect to devd socket\n");
        return -1;
    }

    fcntl(fd, F_SETFD, FD_CLOEXEC);
    md->monitor_fd = fd;
    md->monitor_handle = NULL;
    fprintf(stderr, "isde-mountd: devd monitor active (fd=%d)\n", fd);
    return 0;
}

/* Extract the value of key=... from a devd message into out. */
static int devd_value(const char *msg, const char *key, char *out, size_t len)
{
    size_t klen = strlen(key);
    const char *p = msg;

    while ((p = strstr(p, key)) != NULL) {
        if ((p == msg || p[-1] == ' ' || p[-1] == '!') && p[klen] == '=') {
            p += klen + 1;
            size_t i = 0;
            while (p[i] && p[i] != ' ' && p[i] != '\n' && i < len - 1) {
                out[i] = p[i];
                i++;
            }
            out[i] = '\0';
            return 0;
        }
        p += klen;
    }
    return -1;
}

static void freebsd_monitor_dispatch(MountDaemon *md)
{
    char msg[1024];
    ssize_t n = recv(md->monitor_fd, msg, sizeof(msg) - 1, 0);
    if (n <= 0) {
        return;
    }
    msg[n] = '\0';

    char system[32], subsystem[32], type[32], cdev[128];
    if (devd_value(msg, "system", system, sizeof(system)) != 0 ||
        strcmp(system, "DEVFS") != 0) {
        return;
    }
    if (devd_value(msg, "subsystem", subsystem, sizeof(subsystem)) != 0 ||
        strcmp(subsystem, "CDEV") != 0) {
        return;
    }
    if (devd_value(msg, "type", type, sizeof(type)) != 0) {
        return;
    }
    if (devd_value(msg, "cdev", cdev, sizeof(cdev)) != 0) {
        return;
    }

    char disk[64];
    disk_of(cdev, disk, sizeof(disk));
    if (!is_removable_name(disk)) {
        return;
    }

    char devnode[DEV_PATH_LEN];
    snprintf(devnode, sizeof(devnode), "/dev/%s", cdev);

    fprintf(stderr, "isde-mountd: devd: %s %s\n", type, cdev);

    if (strcmp(type, "CREATE") == 0) {
        if (mountd_find_device(md, devnode)) {
            return;
        }
        int whole = (strcmp(cdev, disk) == 0);
        struct gmesh mesh;
        if (geom_gettree(&mesh) != 0) {
            return;
        }
        add_candidate(md, &mesh, cdev, disk, whole ? 1 : 0, 1);
        geom_deletetree(&mesh);
    } else if (strcmp(type, "DESTROY") == 0) {
        Device *d = mountd_find_device(md, devnode);
        if (d) {
            if (d->is_luks && d->is_unlocked && d->luks_opened_by_us) {
                char closeerr[256];
                mountd_luks_close(d->dm_name, closeerr, sizeof(closeerr));
            }
            d->is_mounted = 0;
            d->mount_point[0] = '\0';
            mountd_dbus_emit_device_removed(md, devnode);
            mountd_remove_device(md, devnode);
        }
    }
}

static void freebsd_monitor_cleanup(MountDaemon *md)
{
    if (md->monitor_fd >= 0) {
        close(md->monitor_fd);
        md->monitor_fd = -1;
    }
    md->monitor_handle = NULL;
}

/* ---------- eject ---------- */

static void disk_node_of(const char *dev_path, char *out, size_t len)
{
    const char *base = strrchr(dev_path, '/');
    base = base ? base + 1 : dev_path;
    char disk[64];
    disk_of(base, disk, sizeof(disk));
    snprintf(out, len, "/dev/%s", disk);
}

static int da_scsi_eject(const char *diskdev)
{
    struct cam_device *cam = cam_open_device(diskdev, O_RDWR);
    if (!cam) {
        return -1;
    }

    union ccb *ccb = cam_getccb(cam);
    if (!ccb) {
        cam_close_device(cam);
        return -1;
    }

    CCB_CLEAR_ALL_EXCEPT_HDR(&ccb->csio);
    scsi_start_stop(&ccb->csio, /*retries*/ 1, NULL, MSG_SIMPLE_Q_TAG,
                    /*start*/ 0, /*load_eject*/ 1, /*immediate*/ 0,
                    SSD_FULL_SIZE, /*timeout ms*/ 30000);

    int ok = 0;
    if (cam_send_ccb(cam, ccb) >= 0 &&
        (ccb->ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP) {
        ok = 1;
    }

    cam_freeccb(ccb);
    cam_close_device(cam);
    return ok ? 0 : -1;
}

static int freebsd_eject(const char *dev_path)
{
    /* Optical media: CDIOCEJECT */
    int fd = open(dev_path, O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
        if (ioctl(fd, CDIOCEJECT) == 0) {
            close(fd);
            return 0;
        }
        close(fd);
    }

    /* USB mass storage: SCSI START STOP UNIT (eject) on the parent disk */
    char diskdev[80];
    disk_node_of(dev_path, diskdev, sizeof(diskdev));
    return da_scsi_eject(diskdev);
}

/* ---------- ops table ---------- */

static const MountdPlatformOps freebsd_ops = {
    .monitor_init      = freebsd_monitor_init,
    .monitor_dispatch  = freebsd_monitor_dispatch,
    .monitor_cleanup   = freebsd_monitor_cleanup,
    .enumerate_devices = freebsd_enumerate_devices,
    .eject             = freebsd_eject,
};

const MountdPlatformOps *mountd_platform_ops(void)
{
    return &freebsd_ops;
}
