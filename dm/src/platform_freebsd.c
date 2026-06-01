/*
 * platform_freebsd.c — FreeBSD platform ops for isde-dm
 *
 * The build forces -D_POSIX_C_SOURCE on every dm source, but FreeBSD's
 * console/evdev headers (<sys/consio.h>, <dev/evdev/input.h>) use BSD types
 * such as u_short that <sys/types.h> only exposes when __BSD_VISIBLE is set —
 * i.e. when no strict POSIX feature macro is defined.  Undefine it here so
 * this translation unit sees the full BSD namespace.
 */
#undef _POSIX_C_SOURCE

#include "platform.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/consio.h>
#include <dev/evdev/input.h>

static int freebsd_shutdown(void)
{
    sync();
    return reboot(RB_POWEROFF);
}

static int freebsd_reboot(void)
{
    sync();
    return reboot(RB_AUTOBOOT);
}

static int freebsd_suspend(void)
{
    /*
     * FreeBSD suspend via ACPI: ioctl on /dev/acpi.
     * ACPIIO_SETSLPSTATE with state 3 = S3 (suspend to RAM).
     */
    int fd = open("/dev/acpi", O_RDWR);
    if (fd < 0) {
        return -1;
    }
    int state = 3;  /* S3 = suspend to RAM */
    int ret = ioctl(fd, 0xc0045107 /* ACPIIO_SETSLPSTATE */, &state);
    close(fd);
    return ret;
}

static int freebsd_vt_activate(int vt)
{
    int fd = open("/dev/ttyv0", O_RDWR | O_NOCTTY);
    if (fd < 0) {
        return -1;
    }
    int ret = ioctl(fd, VT_ACTIVATE, vt);
    close(fd);
    return ret;
}

static int freebsd_vt_wait_active(int vt)
{
    int fd = open("/dev/ttyv0", O_RDWR | O_NOCTTY);
    if (fd < 0) {
        return -1;
    }
    int ret = ioctl(fd, VT_WAITACTIVE, vt);
    close(fd);
    return ret;
}

static const char *freebsd_vt_device_path(int vt, char *buf, size_t buflen)
{
    /* FreeBSD VTs are 0-based hex: ttyv0, ttyv1, ..., ttyvf */
    int n = snprintf(buf, buflen, "/dev/ttyv%x", vt);
    if (n < 0 || (size_t)n >= buflen) {
        return NULL;
    }
    return buf;
}

static int freebsd_lid_open(void)
{
    /* FreeBSD's evdev exposes /dev/input/eventN when the evdev module is
     * loaded; absent that, opendir fails and we report no lid switch. */
    DIR *d = opendir("/dev/input");
    if (!d) {
        return -1;
    }

    struct dirent *de;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "event", 5) != 0) {
            continue;
        }

        char path[256];
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);

        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        /* Check if this device has EV_SW with SW_LID */
        unsigned long sw_bits[(SW_MAX + 7) / 8] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof(sw_bits)), sw_bits) >= 0) {
            if (sw_bits[SW_LID / 8] & (1 << (SW_LID % 8))) {
                closedir(d);
                fprintf(stderr, "isde-dm: lid switch found: %s\n", path);
                return fd;
            }
        }
        close(fd);
    }
    closedir(d);
    return -1;
}

static int freebsd_lid_read(int fd, int *closed)
{
    struct input_event ev;
    while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_SW && ev.code == SW_LID) {
            *closed = ev.value ? 1 : 0;
            return 1;
        }
    }
    return 0;
}

static const DmPlatformOps freebsd_ops = {
    .shutdown       = freebsd_shutdown,
    .reboot         = freebsd_reboot,
    .suspend        = freebsd_suspend,
    .vt_activate    = freebsd_vt_activate,
    .vt_wait_active = freebsd_vt_wait_active,
    .vt_device_path = freebsd_vt_device_path,
    .lid_open       = freebsd_lid_open,
    .lid_read       = freebsd_lid_read,
    .rundir           = "/var/run/isde-dm",
    .runtime_dir_base = "/var/run/xdg",
};

const DmPlatformOps *dm_platform_ops(void)
{
    return &freebsd_ops;
}
