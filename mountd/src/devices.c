#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#endif
/*
 * devices.c — device list management
 */
#include "mountd.h"

#include <stdio.h>
#include <string.h>
#ifndef __linux__
#include <sys/param.h>
#include <sys/mount.h>
#endif

Device *mountd_find_device(MountDaemon *md, const char *dev_path)
{
    for (int i = 0; i < md->ndevices; i++) {
        if (strcmp(md->devices[i].dev_path, dev_path) == 0) {
            return &md->devices[i];
        }
    }
    return NULL;
}

Device *mountd_add_device(MountDaemon *md, const char *dev_path,
                          const char *label, const char *vendor,
                          const char *fs_type, int ejectable)
{
    if (mountd_find_device(md, dev_path)) {
        return NULL;  /* already tracked */
    }
    if (md->ndevices >= MAX_DEVICES) {
        fprintf(stderr, "isde-mountd: too many devices\n");
        return NULL;
    }

    Device *d = &md->devices[md->ndevices++];
    memset(d, 0, sizeof(*d));
    snprintf(d->dev_path, sizeof(d->dev_path), "%s", dev_path);
    snprintf(d->label, sizeof(d->label), "%s",
             (label && label[0]) ? label : "");
    snprintf(d->vendor, sizeof(d->vendor), "%s",
             (vendor && vendor[0]) ? vendor : "");
    snprintf(d->fs_type, sizeof(d->fs_type), "%s",
             fs_type ? fs_type : "");
    d->is_ejectable = ejectable;

    fprintf(stderr, "isde-mountd: tracking %s (%s, %s%s)\n",
            d->dev_path, d->label, d->fs_type,
            d->is_ejectable ? ", ejectable" : "");
    return d;
}

void mountd_remove_device(MountDaemon *md, const char *dev_path)
{
    for (int i = 0; i < md->ndevices; i++) {
        if (strcmp(md->devices[i].dev_path, dev_path) == 0) {
            fprintf(stderr, "isde-mountd: untracking %s\n", dev_path);
            md->ndevices--;
            if (i < md->ndevices) {
                md->devices[i] = md->devices[md->ndevices];
            }
            return;
        }
    }
}

void mountd_refresh_mount_state(MountDaemon *md)
{
    /* Clear all mount states first */
    for (int i = 0; i < md->ndevices; i++) {
        md->devices[i].is_mounted = 0;
        md->devices[i].mount_point[0] = '\0';
    }

    /* Check LUKS unlock state */
    for (int i = 0; i < md->ndevices; i++) {
        Device *d = &md->devices[i];
        if (d->is_luks && d->dm_name[0]) {
            d->is_unlocked = mountd_luks_is_active(d->dm_name);
        }
    }

#ifdef __linux__
    /* Scan /proc/mounts for our tracked devices */
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) {
        return;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char dev[256], mp[512], fs[64];
        if (sscanf(line, "%255s %511s %63s", dev, mp, fs) >= 2) {
            /* Direct device match */
            Device *d = mountd_find_device(md, dev);
            if (d) {
                d->is_mounted = 1;
                snprintf(d->mount_point, sizeof(d->mount_point), "%s", mp);
                continue;
            }
            /* Match dm device for LUKS volumes */
            for (int i = 0; i < md->ndevices; i++) {
                d = &md->devices[i];
                if (d->is_luks && d->dm_path[0] &&
                    strcmp(d->dm_path, dev) == 0) {
                    d->is_mounted = 1;
                    snprintf(d->mount_point, sizeof(d->mount_point),
                             "%s", mp);
                    break;
                }
            }
        }
    }

    fclose(fp);
#else
    /* Query the kernel mount table directly */
    struct statfs *mnts = NULL;
    int count = getmntinfo(&mnts, MNT_NOWAIT);
    for (int i = 0; i < count; i++) {
        const char *dev = mnts[i].f_mntfromname;
        const char *mp  = mnts[i].f_mntonname;

        Device *d = mountd_find_device(md, dev);
        if (d) {
            d->is_mounted = 1;
            snprintf(d->mount_point, sizeof(d->mount_point), "%s", mp);
            continue;
        }
        for (int j = 0; j < md->ndevices; j++) {
            d = &md->devices[j];
            if (d->is_luks && d->dm_path[0] &&
                strcmp(d->dm_path, dev) == 0) {
                d->is_mounted = 1;
                snprintf(d->mount_point, sizeof(d->mount_point), "%s", mp);
                break;
            }
        }
    }
#endif
}
