#define _POSIX_C_SOURCE 200809L
/*
 * main.c — isde-tray-net entry point
 */
#include "tray-net.h"

#include <stdio.h>
#include <signal.h>
#include <unistd.h>

static TrayNet tn;

static void on_signal(int sig)
{
    (void)sig;
    tn.running = 0;
}

int main(int argc, char **argv)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (tray_net_init(&tn, &argc, argv) != 0) {
        fprintf(stderr, "isde-tray-net: failed to initialize\n");
        return 1;
    }

    fprintf(stderr, "isde-tray-net: running\n");
    tray_net_run(&tn);

    int restart = tn.restart;
    tray_net_cleanup(&tn);

    if (restart) {
        fprintf(stderr, "isde-tray-net: restarting for theme change\n");
        execvp(argv[0], argv);
        perror("isde-tray-net: execvp");
    }

    fprintf(stderr, "isde-tray-net: exiting\n");
    return 0;
}
