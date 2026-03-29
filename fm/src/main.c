#define _POSIX_C_SOURCE 200809L
#include "fm.h"
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>

static Fm fm;

static void on_signal(int sig)
{
    (void)sig;
    /* fm might not be initialized yet (app_state NULL before fm_init) */
    if (fm.app_state)
        fm.app_state->running = 0;
    else
        exit(1);
}

int main(int argc, char **argv)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (fm_init(&fm, &argc, argv) != 0) {
        fprintf(stderr, "isde-fm: failed to initialize\n");
        return 1;
    }

    fm_run(&fm);
    fm_cleanup(&fm);
    return 0;
}
