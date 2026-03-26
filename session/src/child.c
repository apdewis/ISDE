#define _POSIX_C_SOURCE 200809L
/*
 * child.c — child process spawning, reaping, and respawning
 */
#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

Child *child_spawn(Session *s, const char *command, int respawn, int is_wm)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("isde-session: fork");
        return NULL;
    }

    if (pid == 0) {
        /* Child process — exec via shell for command parsing */
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        perror("isde-session: exec");
        _exit(127);
    }

    /* Parent — track the child */
    Child *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

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
    for (Child *c = s->children; c; c = c->next)
        if (c->pid == pid) return c;
    return NULL;
}

void child_remove(Session *s, Child *c)
{
    Child **pp = &s->children;
    while (*pp && *pp != c) pp = &(*pp)->next;
    if (*pp) *pp = c->next;
    free(c->command);
    free(c);
}

void child_reap(Session *s)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        Child *c = child_find_pid(s, pid);
        if (!c) continue;

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
            int is_wm = c->is_wm;
            int is_panel = c->is_panel;
            child_remove(s, c);
            Child *nc = child_spawn(s, cmd, 1, is_wm);
            if (nc) nc->is_panel = is_panel;
            free(cmd);
        } else {
            child_remove(s, c);
        }
    }
}

void child_kill_all(Session *s)
{
    /* Send SIGTERM to all children */
    for (Child *c = s->children; c; c = c->next) {
        c->respawn = 0; /* Don't respawn during shutdown */
        kill(c->pid, SIGTERM);
    }

    /* Wait briefly for clean exit */
    usleep(500000);

    /* Force kill any remaining */
    for (Child *c = s->children; c; c = c->next)
        kill(c->pid, SIGKILL);

    /* Reap all */
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0)
        ;

    /* Free the list */
    while (s->children) {
        Child *c = s->children;
        s->children = c->next;
        free(c->command);
        free(c);
    }
}
