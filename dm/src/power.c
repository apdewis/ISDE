#define _POSIX_C_SOURCE 200809L
/*
 * power.c — platform-agnostic power management for isde-dm
 *
 * Handles session teardown and delegates actual power operations
 * to the platform vtable.
 */
#include "dm.h"

#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

void dm_kill_sessions(Dm *dm)
{
    /* Terminate active session */
    if (dm->session_pid > 0) {
        kill(dm->session_pid, SIGTERM);
    }

    /* Terminate greeter */
    if (dm->greeter_pid > 0) {
        kill(dm->greeter_pid, SIGTERM);
    }

    /* Brief wait for graceful shutdown */
    struct timespec ts = { .tv_sec = 2, .tv_nsec = 0 };
    nanosleep(&ts, NULL);

    /* Force kill any survivors */
    if (dm->session_pid > 0) {
        kill(dm->session_pid, SIGKILL);
    }
    if (dm->greeter_pid > 0) {
        kill(dm->greeter_pid, SIGKILL);
    }
}

int dm_power_shutdown(Dm *dm)
{
    fprintf(stderr, "isde-dm: shutdown requested\n");
    dm_kill_sessions(dm);
    dm_xserver_stop(dm);
    dm->running = 0;
    return dm->plat->shutdown();
}

int dm_power_reboot(Dm *dm)
{
    fprintf(stderr, "isde-dm: reboot requested\n");
    dm_kill_sessions(dm);
    dm_xserver_stop(dm);
    dm->running = 0;
    return dm->plat->reboot();
}

int dm_power_suspend(Dm *dm)
{
    fprintf(stderr, "isde-dm: suspend requested\n");

    /* Disable seat before suspend */
    if (dm->seat) {
        libseat_disable_seat(dm->seat);
    }

    int ret = dm->plat->suspend();

    /* On resume, re-enable seat */
    if (dm->seat) {
        /* Dispatch to process enable_seat callback */
        libseat_dispatch(dm->seat, 0);
    }

    return ret;
}
