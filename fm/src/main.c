#define _POSIX_C_SOURCE 200809L
#include "fm.h"
#include <stdio.h>
#include <signal.h>

static FmApp app;

static void on_signal(int sig)
{
    (void)sig;
    app.running = 0;
}

int main(int argc, char **argv)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (fm_app_init(&app, &argc, argv) != 0) {
        fprintf(stderr, "isde-fm: failed to initialize\n");
        return 1;
    }

    fm_app_run(&app);
    fm_app_cleanup(&app);
    return 0;
}
