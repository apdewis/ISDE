#define _POSIX_C_SOURCE 200809L
/*
 * child.c — child process spawning, reaping, and respawning
 */
#include "session.h"
#include "isde/isde-ewmh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>

Child *child_spawn(Session *s, const char *command, int respawn, int is_wm)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("isde-session: fork");
        return NULL;
    }

    if (pid == 0) {
        /* Close the write end of the death pipe — only the parent holds it.
         * The read end stays open; EOF signals parent death. */
        if (s->death_pipe[1] >= 0) {
            close(s->death_pipe[1]);
        }

        /* Child process — exec via shell for command parsing */
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        perror("isde-session: exec");
        _exit(127);
    }

    /* Parent — track the child */
    Child *c = calloc(1, sizeof(*c));
    if (!c) { return NULL; }

    c->pid     = pid;
    c->command = strdup(command);
    c->respawn = respawn;
    c->is_wm   = is_wm;

    c->next = s->children;
    s->children = c;

    return c;
}

Child *child_find_pid(Session *s, pid_t pid)
{
    for (Child *c = s->children; c; c = c->next) {
        if (c->pid == pid) { return c; }
    }
    return NULL;
}

void child_remove(Session *s, Child *c)
{
    Child **pp = &s->children;
    while (*pp && *pp != c) { pp = &(*pp)->next; }
    if (*pp) { *pp = c->next; }
    free(c->command);
    free(c);
}

void child_reap(Session *s)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        Child *c = child_find_pid(s, pid);
        if (!c) { continue; }

        if (WIFEXITED(status)) {
            fprintf(stderr, "isde-session: '%s' exited (status %d)\n",
                    c->command, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "isde-session: '%s' killed (signal %d)\n",
                    c->command, WTERMSIG(status));
        }

        if (c->respawn && s->running) {
            fprintf(stderr, "isde-session: respawning '%s'\n", c->command);
            char *cmd = strdup(c->command);
            int wm = c->is_wm;
            int panel = c->is_panel;
            child_remove(s, c);
            Child *nc = child_spawn(s, cmd, 1, wm);
            if (nc) { nc->is_panel = panel; }
            free(cmd);
        } else {
            child_remove(s, c);
        }
    }
}

void child_kill_all(Session *s)
{
    struct timespec ts = { 0, 50000000 }; /* 50ms */

    /* Disable respawning */
    for (Child *c = s->children; c; c = c->next) {
        c->respawn = 0;
    }

    /* Phase 0: ask the WM to close every managed client via _NET_CLOSE_WINDOW.
     * Daemonized apps (VSCodium, Electron apps that call setsid) escape our
     * process group, so kill(0, SIGTERM) alone misses them. Closing them by
     * their top-level window goes through the WM regardless of PID lineage
     * and also gives apps a chance to save state before exiting. */
    if (s->conn && !xcb_connection_has_error(s->conn)) {
        IsdeEwmh *ewmh = isde_ewmh_init(s->conn, s->screen_num);
        if (ewmh) {
            xcb_window_t *wins = NULL;
            int n = isde_ewmh_get_client_list(ewmh, &wins);
            for (int i = 0; i < n; i++) {
                isde_ewmh_request_close_window(ewmh, wins[i]);
            }
            free(wins);
            xcb_flush(s->conn);
            isde_ewmh_free(ewmh);

            /* Give apps up to ~3 seconds to save state and exit cleanly. */
            for (int i = 0; i < 60; i++) {
                int status;
                pid_t pid;
                while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                    Child *c = child_find_pid(s, pid);
                    if (c) { child_remove(s, c); }
                }
                nanosleep(&ts, NULL);
            }
        }
    }

    /* Phase 1: SIGTERM everything in our process group */
    kill(0, SIGTERM);

    /* Phase 2: Poll for up to 2 seconds, reaping as children exit */
    for (int i = 0; i < 40 && s->children; i++) {
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            Child *c = child_find_pid(s, pid);
            if (c) { child_remove(s, c); }
        }
        if (!s->children) { break; }
        nanosleep(&ts, NULL);
    }

    /* Phase 3: SIGKILL everything in our process group, including us.
     * Our job is done — the parent (display manager/init) reaps us. */
    if (s->children) {
        kill(0, SIGKILL);
        /* unreachable */
    }

    /* All children exited cleanly — free any remaining list entries */
    while (s->children) {
        Child *c = s->children;
        s->children = c->next;
        free(c->command);
        free(c);
    }
}
