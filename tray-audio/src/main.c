#define _POSIX_C_SOURCE 200809L
/*
 * main.c — isde-tray-audio entry point
 */
#include "tray-audio.h"

#include <stdio.h>
#include <signal.h>
#include <unistd.h>

static TrayAudio ta;

static void on_signal(int sig)
{
    (void)sig;
    ta.running = 0;
}

int main(int argc, char **argv)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (tray_audio_init(&ta, &argc, argv) != 0) {
        fprintf(stderr, "isde-tray-audio: failed to initialize\n");
        return 1;
    }

    fprintf(stderr, "isde-tray-audio: running\n");
    tray_audio_run(&ta);

    int restart = ta.restart;
    tray_audio_cleanup(&ta);

    if (restart) {
        fprintf(stderr, "isde-tray-audio: restarting for theme change\n");
        execvp(argv[0], argv);
        perror("isde-tray-audio: execvp");
    }

    fprintf(stderr, "isde-tray-audio: exiting\n");
    return 0;
}
