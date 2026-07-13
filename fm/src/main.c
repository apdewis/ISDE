#define _POSIX_C_SOURCE 200809L
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>

static FmApp app;

static void on_signal(int sig)
{
    (void)sig;
    app.running = 0;
}

/* ---------- argv helpers ---------- */

static int has_flag(int argc, char **argv, const char *flag)
{
    for (int i = 1; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], flag) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ---------- daemonization ---------- */

static void daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("isde-fm: fork");
        exit(1);
    }
    if (pid > 0) {
        /* Parent exits. */
        _exit(0);
    }
    if (setsid() < 0) {
        perror("isde-fm: setsid");
        exit(1);
    }
    /* Second fork to ensure the daemon cannot reacquire a controlling tty. */
    pid = fork();
    if (pid < 0) {
        perror("isde-fm: fork");
        exit(1);
    }
    if (pid > 0) {
        _exit(0);
    }
    umask(0);
    chdir("/");

    /* Redirect stdio to /dev/null. */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) {
            close(devnull);
        }
    }
}

/* ---------- launcher path (D-Bus client) ---------- */

static char *resolve_path(int argc, char **argv)
{
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i] && argv[i][0] != '-') {
            path = argv[i];
            break;
        }
    }
    if (!path) {
        path = getenv("HOME");
        if (!path || !path[0]) {
            path = "/";
        }
    }
    if (strncmp(path, "file://", 7) == 0) {
        path += 7;
        if (path[0] == '\0') {
            path = "/";
        }
    }
    char *resolved = realpath(path, NULL);
    return resolved ? resolved : strdup(path);
}

static int spawn_daemon(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("isde-fm: fork");
        return -1;
    }
    if (pid == 0) {
        /* Child: become the daemon. */
        execlp("isde-fm", "isde-fm", "--background", (char *)NULL);
        perror("isde-fm: exec isde-fm --background");
        _exit(127);
    }
    /* Reap the intermediate child (the daemon double-forks away). */
    int status;
    waitpid(pid, &status, 0);
    return 0;
}

static int wait_for_name(IsdeDBus *bus, const char *name)
{
    for (int i = 0; i < 40; i++) {  /* 40 * 50ms = 2s */
        if (fm_name_has_owner(bus, name)) {
            return 0;
        }
        usleep(50 * 1000);
    }
    return -1;
}

static int launcher_main(int argc, char **argv)
{
    IsdeDBus *bus = isde_dbus_init();
    if (!bus) {
        fprintf(stderr, "isde-fm: cannot connect to D-Bus session bus\n");
        return 1;
    }

    char *path = resolve_path(argc, argv);
    const char *name = fm_dbus_name();

    /* Ensure a daemon is running. */
    if (!fm_name_has_owner(bus, name)) {
        if (spawn_daemon() < 0) {
            isde_dbus_free(bus);
            free(path);
            return 1;
        }
        if (wait_for_name(bus, name) < 0) {
            fprintf(stderr, "isde-fm: daemon failed to start\n");
            isde_dbus_free(bus);
            free(path);
            return 1;
        }
    }

    /* Call OpenPath; retry once if the daemon died mid-call. */
    if (fm_call_open_path(bus, name, path) < 0) {
        if (spawn_daemon() < 0 ||
            wait_for_name(bus, name) < 0 ||
            fm_call_open_path(bus, name, path) < 0) {
            fprintf(stderr, "isde-fm: failed to open path via daemon\n");
            isde_dbus_free(bus);
            free(path);
            return 1;
        }
    }

    isde_dbus_free(bus);
    free(path);
    return 0;
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int background = has_flag(argc, argv, "--background");

    if (!background) {
        return launcher_main(argc, argv);
    }

    /* Daemon path: fm_app_init strips --background and sets app->background.
     * Daemonize first, then run the app context. */
    daemonize();

    int rc = fm_app_init(&app, &argc, argv);
    if (rc < 0) {
        fm_app_cleanup(&app);
        return 1;
    }
    if (rc == 1) {
        /* Another daemon already owns the name — exit cleanly. */
        fm_app_cleanup(&app);
        return 0;
    }

    fm_app_run(&app);
    fm_app_cleanup(&app);
    return 0;
}
