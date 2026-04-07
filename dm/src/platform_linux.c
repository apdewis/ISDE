#define _POSIX_C_SOURCE 200809L
/*
 * platform_linux.c — Linux platform ops for isde-dm
 */
#include "platform.h"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <linux/vt.h>

static int linux_shutdown(void)
{
    sync();
    return reboot(RB_POWER_OFF);
}

static int linux_reboot(void)
{
    sync();
    return reboot(RB_AUTOBOOT);
}

static int linux_suspend(void)
{
    int fd = open("/sys/power/state", O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    ssize_t n = write(fd, "mem", 3);
    close(fd);
    return n == 3 ? 0 : -1;
}

static int linux_vt_activate(int vt)
{
    int fd = open("/dev/tty0", O_RDWR | O_NOCTTY);
    if (fd < 0) {
        return -1;
    }
    int ret = ioctl(fd, VT_ACTIVATE, vt);
    close(fd);
    return ret;
}

static int linux_vt_wait_active(int vt)
{
    int fd = open("/dev/tty0", O_RDWR | O_NOCTTY);
    if (fd < 0) {
        return -1;
    }
    int ret = ioctl(fd, VT_WAITACTIVE, vt);
    close(fd);
    return ret;
}

static const char *linux_vt_device_path(int vt, char *buf, size_t buflen)
{
    int n = snprintf(buf, buflen, "/dev/tty%d", vt);
    if (n < 0 || (size_t)n >= buflen) {
        return NULL;
    }
    return buf;
}

static const DmPlatformOps linux_ops = {
    .shutdown       = linux_shutdown,
    .reboot         = linux_reboot,
    .suspend        = linux_suspend,
    .vt_activate    = linux_vt_activate,
    .vt_wait_active = linux_vt_wait_active,
    .vt_device_path = linux_vt_device_path,
    .rundir         = "/run/isde-dm",
};

const DmPlatformOps *dm_platform_ops(void)
{
    return &linux_ops;
}
