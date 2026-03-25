#define _POSIX_C_SOURCE 200809L
/*
 * main.c — isde-panel entry point
 */
#include "panel.h"

#include <stdio.h>
#include <signal.h>

static Panel panel;

static void on_signal(int sig)
{
    (void)sig;
    panel.running = 0;
}

int main(int argc, char **argv)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (panel_init(&panel, &argc, argv) != 0) {
        fprintf(stderr, "isde-panel: failed to initialize\n");
        return 1;
    }

    fprintf(stderr, "isde-panel: running\n");
    panel_run(&panel);

    panel_cleanup(&panel);
    fprintf(stderr, "isde-panel: exiting\n");
    return 0;
}
