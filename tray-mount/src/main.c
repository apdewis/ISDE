#define _POSIX_C_SOURCE 200809L
/*
 * main.c — isde-tray-mount entry point
 */
#include "tray-mount.h"

#include <stdio.h>
#include <signal.h>

static TrayMount tm;

static void on_signal(int sig)
{
    (void)sig;
    tm.running = 0;
}

int main(int argc, char **argv)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (tray_mount_init(&tm, &argc, argv) != 0) {
        fprintf(stderr, "isde-tray-mount: failed to initialize\n");
        return 1;
    }

    fprintf(stderr, "isde-tray-mount: running\n");
    tray_mount_run(&tm);

    tray_mount_cleanup(&tm);
    fprintf(stderr, "isde-tray-mount: exiting\n");
    return 0;
}
