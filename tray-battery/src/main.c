#define _POSIX_C_SOURCE 200809L
/*
 * main.c — isde-tray-battery entry point
 */
#include "tray-battery.h"

#include <stdio.h>
#include <signal.h>

static TrayBattery tb;

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
        if (tray_battery_init(&tb, &argc, argv) != 0) {
            fprintf(stderr, "isde-tray-battery: failed to initialize\n");
            return 1;
        }

        fprintf(stderr, "isde-tray-battery: running\n");
        tray_battery_run(&tb);

        int restart = tb.restart;
        tray_battery_cleanup(&tb);

        if (!restart) {
            break;
        }

        fprintf(stderr, "isde-tray-battery: restarting\n");
    } while (1);

    fprintf(stderr, "isde-tray-battery: exiting\n");
    return 0;
}
