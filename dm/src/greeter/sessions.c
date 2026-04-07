#define _POSIX_C_SOURCE 200809L
/*
 * sessions.c — scan /usr/share/xsessions/ for available sessions
 */
#include "greeter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

void greeter_sessions_load(Greeter *g)
{
    const char *dir = "/usr/share/xsessions";

    /* Scan the directory ourselves to get filenames */
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "isde-greeter: cannot open %s\n", dir);
        g->sessions = malloc(sizeof(GreeterSession));
        g->sessions[0].name = strdup("Default");
        g->sessions[0].desktop_file = strdup("isde.desktop");
        g->nsessions = 1;
        g->active_session = 0;
        return;
    }

    g->sessions = NULL;
    g->nsessions = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t nlen = strlen(de->d_name);
        if (nlen < 9 || strcmp(de->d_name + nlen - 8, ".desktop") != 0) {
            continue;
        }

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);

        IsdeDesktopEntry *entry = isde_desktop_load(path);
        if (!entry) {
            continue;
        }

        const char *name = isde_desktop_name(entry);
        if (!name) {
            isde_desktop_free(entry);
            continue;
        }

        g->sessions = realloc(g->sessions,
                              (g->nsessions + 1) * sizeof(GreeterSession));
        g->sessions[g->nsessions].name = strdup(name);
        g->sessions[g->nsessions].desktop_file = strdup(de->d_name);
        g->nsessions++;

        isde_desktop_free(entry);
    }
    closedir(d);

    if (g->nsessions == 0) {
        fprintf(stderr, "isde-greeter: no sessions found in %s\n", dir);
        g->sessions = malloc(sizeof(GreeterSession));
        g->sessions[0].name = strdup("Default");
        g->sessions[0].desktop_file = strdup("isde.desktop");
        g->nsessions = 1;
    }

    /* Default to first session */
    g->active_session = 0;

    fprintf(stderr, "isde-greeter: loaded %d session(s)\n", g->nsessions);
}

void greeter_sessions_cleanup(Greeter *g)
{
    for (int i = 0; i < g->nsessions; i++) {
        free(g->sessions[i].name);
        free(g->sessions[i].desktop_file);
    }
    free(g->sessions);
    g->sessions = NULL;
    g->nsessions = 0;
}
