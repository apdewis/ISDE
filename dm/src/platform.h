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

    /* Return the device path for a VT number.
     * Writes into buf (buflen bytes).  Returns buf on success, NULL on error. */
    const char *(*vt_device_path)(int vt, char *buf, size_t buflen);

    /* Platform paths */
    const char *rundir;           /* "/run/isde-dm" or "/var/run/isde-dm" */
} DmPlatformOps;

/* Returns the ops table for the compiled platform. */
const DmPlatformOps *dm_platform_ops(void);

#endif /* DM_PLATFORM_H */
