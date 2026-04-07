#define _POSIX_C_SOURCE 200809L
/*
 * greeter_proc.c — greeter process management
 */
#include "dm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pwd.h>
#include <time.h>

int dm_greeter_start(Dm *dm)
{
    if (dm->greeter_pid > 0) {
        return 0;  /* already running */
    }

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        /* Child: exec the greeter */

        /* Set up environment */
        setenv("DISPLAY", dm->display, 1);
        setenv("XAUTHORITY", dm->xauth_path, 1);

        /* Set the IPC socket path */
        char sock_path[512];
        snprintf(sock_path, sizeof(sock_path), "%s/greeter.sock",
                 dm->plat->rundir);
        setenv("ISDE_DM_SOCK", sock_path, 1);

        /*
         * TODO: drop privileges to isde-dm user.
         * For now, run as root (same as X server).
         * In production, look up uid/gid for "isde-dm" user
         * and setuid/setgid before exec.
         */

        execlp(dm->greeter_cmd, dm->greeter_cmd, (char *)NULL);
        fprintf(stderr, "isde-dm: exec greeter '%s' failed: %s\n",
                dm->greeter_cmd, strerror(errno));
        _exit(1);
    }

    dm->greeter_pid = pid;
    fprintf(stderr, "isde-dm: greeter started (pid %d)\n", pid);
    return 0;
}

void dm_greeter_stop(Dm *dm)
{
    if (dm->greeter_pid <= 0) {
        return;
    }

    kill(dm->greeter_pid, SIGTERM);

    /* Wait briefly for graceful exit */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 500000000 };
    nanosleep(&ts, NULL);

    if (dm->greeter_pid > 0) {
        kill(dm->greeter_pid, SIGKILL);
    }

    /* Close IPC client connection if any */
    if (dm->ipc_client_fd >= 0) {
        close(dm->ipc_client_fd);
        dm->ipc_client_fd = -1;
        dm->ipc_buf_len = 0;
    }
}
