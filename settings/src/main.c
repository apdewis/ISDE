#define _POSIX_C_SOURCE 200809L
#include "settings.h"
#include <stdio.h>
#include <signal.h>

static Settings settings;

static void on_signal(int sig)
{
    (void)sig;
    settings.running = 0;
}

int main(int argc, char **argv)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (settings_init(&settings, &argc, argv) != 0) {
        fprintf(stderr, "isde-settings: failed to initialize\n");
        return 1;
    }

    settings_run(&settings);
    settings_cleanup(&settings);
    return 0;
}
