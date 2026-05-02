#define _POSIX_C_SOURCE 200809L
/*
 * main.c — isde-tray-bt entry point
 */
#include "tray-bt.h"

#include <stdio.h>
#include <signal.h>

static TrayBt tb;

static void on_signal(int sig)
{
    (void)sig;
    tb.running = 0;
}

int main(int argc, char **argv)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    do {
        if (tray_bt_init(&tb, &argc, argv) != 0) {
            fprintf(stderr, "isde-tray-bt: failed to initialize\n");
            return 1;
        }

        fprintf(stderr, "isde-tray-bt: running\n");
        tray_bt_run(&tb);

        int restart = tb.restart;
        tray_bt_cleanup(&tb);

        if (!restart) {
            break;
        }

        fprintf(stderr, "isde-tray-bt: restarting\n");
    } while (1);

    fprintf(stderr, "isde-tray-bt: exiting\n");
    return 0;
}
