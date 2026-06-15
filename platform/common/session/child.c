#define _POSIX_C_SOURCE 200809L
/*
 * child.c — child process spawning, reaping, and respawning
 */
#include "session.h"
#include "child.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>

#define RAPID_CRASH_WINDOW_MS  5000
#define MAX_RAPID_CRASHES      5
#define BASE_RETRY_DELAY_MS    1000
#define MAX_RETRY_DELAY_MS     60000

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

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

    c->pid      = pid;
    c->command  = strdup(command);
    c->respawn  = respawn;
    c->is_wm    = is_wm;
    c->start_ms = now_ms();

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
            long long uptime = now_ms() - c->start_ms;
            int crashes = c->rapid_crashes;
            if (uptime < RAPID_CRASH_WINDOW_MS) {
                crashes++;
            } else {
                crashes = 0;
            }

            if (crashes >= MAX_RAPID_CRASHES) {
                fprintf(stderr, "isde-session: '%s' crashed %d times "
                        "rapidly, giving up\n", c->command, crashes);
                child_remove(s, c);
            } else if (crashes > 0) {
                long long delay = BASE_RETRY_DELAY_MS << (crashes - 1);
                if (delay > MAX_RETRY_DELAY_MS) {
                    delay = MAX_RETRY_DELAY_MS;
                }
                fprintf(stderr, "isde-session: '%s' crashed after %lldms, "
                        "retry #%d in %lldms\n",
                        c->command, uptime, crashes, delay);
                PendingRespawn *pr = calloc(1, sizeof(*pr));
                pr->command = strdup(c->command);
                pr->is_wm = c->is_wm;
                pr->is_panel = c->is_panel;
                pr->rapid_crashes = crashes;
                pr->deadline_ms = now_ms() + delay;
                pr->next = s->pending_respawns;
                s->pending_respawns = pr;
                child_remove(s, c);
            } else {
                fprintf(stderr, "isde-session: respawning '%s'\n",
                        c->command);
                char *cmd = strdup(c->command);
                int wm = c->is_wm;
                int panel = c->is_panel;
                child_remove(s, c);
                Child *nc = child_spawn(s, cmd, 1, wm);
                if (nc) { nc->is_panel = panel; }
                free(cmd);
            }
        } else {
            child_remove(s, c);
        }
    }
}

void child_process_pending_respawns(Session *s)
{
    long long now = now_ms();
    PendingRespawn **pp = &s->pending_respawns;
    while (*pp) {
        PendingRespawn *pr = *pp;
        if (now < pr->deadline_ms) {
            pp = &pr->next;
            continue;
        }
        *pp = pr->next;
        fprintf(stderr, "isde-session: delayed respawn '%s'\n", pr->command);
        Child *nc = child_spawn(s, pr->command, 1, pr->is_wm);
        if (nc) {
            nc->is_panel = pr->is_panel;
            nc->rapid_crashes = pr->rapid_crashes;
        }
        free(pr->command);
        free(pr);
    }
}

long long child_next_respawn_deadline(Session *s)
{
    long long earliest = -1;
    for (PendingRespawn *pr = s->pending_respawns; pr; pr = pr->next) {
        if (earliest < 0 || pr->deadline_ms < earliest) {
            earliest = pr->deadline_ms;
        }
    }
    return earliest;
}

void restart_ui_children(Session *s)
{
    /* SIGTERM the WM and panel — child_reap will respawn them */
    for (Child *c = s->children; c; c = c->next) {
        if (c->is_wm || c->is_panel) {
            kill(c->pid, SIGTERM);
        }
    }
}