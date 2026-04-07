#define _POSIX_C_SOURCE 200809L
/*
 * main.c — isde-dm entry point
 */
#include "dm.h"

#include <stdio.h>
#include <signal.h>
#include <unistd.h>

static Dm dm;

static void on_signal(int sig)
{
    (void)sig;
    dm.running = 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (dm_init(&dm) != 0) {
        fprintf(stderr, "isde-dm: failed to initialize\n");
        return 1;
    }

    fprintf(stderr, "isde-dm: running\n");
    dm_run(&dm);

    dm_cleanup(&dm);
    fprintf(stderr, "isde-dm: exiting\n");
    return 0;
}
