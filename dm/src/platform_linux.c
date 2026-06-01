#define _POSIX_C_SOURCE 200809L
/*
 * platform_linux.c — Linux platform ops for isde-dm
 */
#include "platform.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <linux/vt.h>
#include <linux/input.h>

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

static int linux_lid_open(void)
{
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

static int linux_lid_read(int fd, int *closed)
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

static const DmPlatformOps linux_ops = {
    .shutdown       = linux_shutdown,
    .reboot         = linux_reboot,
    .suspend        = linux_suspend,
    .vt_activate    = linux_vt_activate,
    .vt_wait_active = linux_vt_wait_active,
    .vt_device_path = linux_vt_device_path,
    .lid_open       = linux_lid_open,
    .lid_read       = linux_lid_read,
    .rundir           = "/run/isde-dm",
    .runtime_dir_base = "/run/user",
};

const DmPlatformOps *dm_platform_ops(void)
{
    return &linux_ops;
}
