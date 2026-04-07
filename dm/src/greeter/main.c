#define _POSIX_C_SOURCE 200809L
/*
 * main.c — isde-greeter entry point
 */
#include "greeter.h"

#include <stdio.h>
#include <string.h>
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

    /* Extract --lock <username> before Xt sees it (Xt rejects unknown options) */
    const char *lock_user = NULL;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--lock") == 0) {
            lock_user = argv[i + 1];
            /* Remove --lock and username from argv */
            int remaining = argc - i - 2;
            memmove(&argv[i], &argv[i + 2], remaining * sizeof(char *));
            argc -= 2;
            argv[argc] = NULL;
            break;
        }
    }

    if (greeter_init(&greeter, &argc, argv) != 0) {
        fprintf(stderr, "isde-greeter: failed to initialize\n");
        return 1;
    }

    if (lock_user) {
        greeter_enter_lock_mode(&greeter, lock_user);
    }

    fprintf(stderr, "isde-greeter: running\n");
    greeter_run(&greeter);

    greeter_cleanup(&greeter);
    fprintf(stderr, "isde-greeter: exiting\n");
    return 0;
}
