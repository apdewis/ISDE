#define _POSIX_C_SOURCE 200809L
/*
 * main.c — isde-wm entry point
 */
#include "wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

static Wm wm;

static void on_signal(int sig)
{
    (void)sig;
    wm.running = 0;
}

int main(int argc, char **argv)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    do {
        if (wm_init(&wm, &argc, argv) != 0) {
            fprintf(stderr, "isde-wm: failed to initialize\n");
            return 1;
        }

        fprintf(stderr, "isde-wm: running\n");
        wm_run(&wm);

        int restart = wm.restart;
        wm_cleanup(&wm);

        if (!restart)
            break;

        fprintf(stderr, "isde-wm: restarting\n");
    } while (1);

    fprintf(stderr, "isde-wm: exiting\n");
    return 0;
}
