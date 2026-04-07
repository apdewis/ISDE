#define _POSIX_C_SOURCE 200809L
/*
 * main.c — isde-greeter entry point
 */
#include "greeter.h"

#include <stdio.h>
#include <signal.h>

static Greeter greeter;

static void on_signal(int sig)
{
    (void)sig;
    greeter.running = 0;
}

int main(int argc, char **argv)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (greeter_init(&greeter, &argc, argv) != 0) {
        fprintf(stderr, "isde-greeter: failed to initialize\n");
        return 1;
    }

    fprintf(stderr, "isde-greeter: running\n");
    greeter_run(&greeter);

    greeter_cleanup(&greeter);
    fprintf(stderr, "isde-greeter: exiting\n");
    return 0;
}
