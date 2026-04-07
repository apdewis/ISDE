#define _POSIX_C_SOURCE 200809L
/*
 * platform_freebsd.c — FreeBSD platform ops for isde-dm
 */
#include "platform.h"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/consio.h>

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

static const DmPlatformOps freebsd_ops = {
    .shutdown       = freebsd_shutdown,
    .reboot         = freebsd_reboot,
    .suspend        = freebsd_suspend,
    .vt_activate    = freebsd_vt_activate,
    .vt_wait_active = freebsd_vt_wait_active,
    .vt_device_path = freebsd_vt_device_path,
    .rundir         = "/var/run/isde-dm",
};

const DmPlatformOps *dm_platform_ops(void)
{
    return &freebsd_ops;
}
