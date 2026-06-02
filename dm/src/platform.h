/*
 * platform.h — OS portability vtable for isde-dm
 *
 * Each supported OS provides a platform_<os>.c implementing these ops.
 * CMake selects which file to compile.  Mainline daemon code calls
 * through dm_platform_ops() — no #ifdef blocks elsewhere.
 */
#ifndef DM_PLATFORM_H
#define DM_PLATFORM_H

#include <stddef.h>

typedef struct DmPlatformOps {
    /* Power management — return 0 on success, -1 on failure */
    int  (*shutdown)(void);
    int  (*reboot)(void);
    int  (*suspend)(void);

    /* VT management (fallback when libseat is insufficient) */
    int  (*vt_activate)(int vt);
    int  (*vt_wait_active)(int vt);

    /* VT to run the X server on (1-based, matching Xorg's "vtN" argument and
     * the VT_ACTIVATE ioctl index).  Linux: 7 (the traditional free tty7).
     * FreeBSD: 9, i.e. /dev/ttyv8 — ttyv0..ttyv7 carry gettys from /etc/ttys. */
    int xserver_vt;

    /* Return the device path for a VT number.
     * Writes into buf (buflen bytes).  Returns buf on success, NULL on error. */
    const char *(*vt_device_path)(int vt, char *buf, size_t buflen);

    /* Laptop lid switch.
     * lid_open() returns an open fd for the lid device, or -1 if none.
     * lid_read() reads the next lid-state change from fd: returns 1 and sets
     * *closed (1=closed, 0=open) when a lid event is available, or 0 when the
     * fd is drained of lid events. */
    int  (*lid_open)(void);
    int  (*lid_read)(int fd, int *closed);

    /* Platform paths */
    const char *rundir;           /* "/run/isde-dm" or "/var/run/isde-dm" */

    /* Base directory for the user session's XDG_RUNTIME_DIR; the per-user
     * directory is "<runtime_dir_base>/<uid>" ("/run/user" on Linux,
     * "/var/run/xdg" on FreeBSD). */
    const char *runtime_dir_base;
} DmPlatformOps;

/* Returns the ops table for the compiled platform. */
const DmPlatformOps *dm_platform_ops(void);

#endif /* DM_PLATFORM_H */
