#define _POSIX_C_SOURCE 200809L
/*
 * main.c — isde-mountd entry point
 */
#include "mountd.h"

#include <stdio.h>
#include <signal.h>

static MountDaemon md;

static void on_signal(int sig)
{
    (void)sig;
    md.running = 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (mountd_init(&md) != 0) {
        fprintf(stderr, "isde-mountd: failed to initialize\n");
        return 1;
    }

    fprintf(stderr, "isde-mountd: running\n");
    mountd_run(&md);

    mountd_cleanup(&md);
    fprintf(stderr, "isde-mountd: exiting\n");
    return 0;
}
