#define _POSIX_C_SOURCE 200809L
/*
 * main.c — isde-session entry point
 */
#include "session.h"

#include <stdio.h>
#include <signal.h>

static Session session;

static void on_signal(int sig)
{
    (void)sig;
    session.running = 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (session_init(&session) != 0) {
        fprintf(stderr, "isde-session: failed to initialize\n");
        return 1;
    }

    fprintf(stderr, "isde-session: running\n");
    session_run(&session);

    session_cleanup(&session);
    fprintf(stderr, "isde-session: exiting\n");
    return 0;
}
