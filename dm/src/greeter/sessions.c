#define _POSIX_C_SOURCE 200809L
/*
 * sessions.c — scan <datadir>/xsessions for available sessions
 */
#include "greeter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void greeter_sessions_load(Greeter *g)
{
    g->sessions = NULL;
    g->nsessions = 0;

    /* Scan <datadir>/xsessions across XDG_DATA_DIRS (FreeBSD ports install
     * under /usr/local/share, not /usr/share). */
    const char *dp = isde_xdg_data_dirs();
    while (dp && *dp) {
        const char *colon = strchr(dp, ':');
        size_t dlen = colon ? (size_t)(colon - dp) : strlen(dp);
        if (dlen > 0) {
            char dir[512];
            snprintf(dir, sizeof(dir), "%.*s/xsessions", (int)dlen, dp);
            int count = 0;
            IsdeDesktopEntry **entries = isde_desktop_scan_dir(dir, &count);
            for (int i = 0; i < count; i++) {
                const char *name = isde_desktop_name(entries[i]);
                const char *file = isde_desktop_id(entries[i]);
                if (name && file) {
                    g->sessions = realloc(g->sessions,
                        (g->nsessions + 1) * sizeof(GreeterSession));
                    g->sessions[g->nsessions].name = strdup(name);
                    g->sessions[g->nsessions].desktop_file = strdup(file);
                    g->nsessions++;
                }
                isde_desktop_free(entries[i]);
            }
            free(entries);
        }
        dp = colon ? colon + 1 : NULL;
    }

    if (g->nsessions == 0) {
        fprintf(stderr, "isde-greeter: no sessions found\n");
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
    free(g->session_names);
    g->session_names = NULL;
    for (int i = 0; i < g->nsessions; i++) {
        free(g->sessions[i].name);
        free(g->sessions[i].desktop_file);
    }
    free(g->sessions);
    g->sessions = NULL;
    g->nsessions = 0;
}
