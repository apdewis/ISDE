/*
 * platform.h — OS portability vtable for isde-mountd
 *
 * Each supported OS provides a platform_<os>.c implementing these ops.
 * CMake selects which file to compile.
 */
#ifndef MOUNTD_PLATFORM_H
#define MOUNTD_PLATFORM_H

typedef struct MountDaemon MountDaemon;

typedef struct MountdPlatformOps {
    /* Initialize device monitoring.  Returns 0 on success, -1 on failure.
     * Sets md->monitor_fd and md->monitor_handle. */
    int  (*monitor_init)(MountDaemon *md);

    /* Process pending device events from monitor_fd. */
    void (*monitor_dispatch)(MountDaemon *md);

    /* Clean up monitor resources. */
    void (*monitor_cleanup)(MountDaemon *md);

    /* Enumerate currently attached removable devices and populate md->devices. */
    void (*enumerate_devices)(MountDaemon *md);

    /* Eject a device (platform-specific ioctl/sysfs). Returns 0 or -1. */
    int  (*eject)(const char *dev_path);
} MountdPlatformOps;

/* Returns the ops table for the compiled platform. */
const MountdPlatformOps *mountd_platform_ops(void);

#endif /* MOUNTD_PLATFORM_H */
