#define _POSIX_C_SOURCE 200809L
/*
 * autostart.c — autostart file parsing
 *
 * Reads the ISDE autostart file ($XDG_CONFIG_HOME/isde/autostart):
 *   - One command per line
 *   - Lines starting with @ are respawned on crash
 *   - Lines starting with # are comments
 *   - Empty lines are skipped
 *
 * Also reads freedesktop.org autostart .desktop files from
 * $XDG_CONFIG_DIRS/autostart/ per the Desktop Application Autostart spec.
 */
#include "session.h"
#include "isde/isde-desktop.h"
#include "isde/isde-xdg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void add_entry(Session *s, const char *cmd, int respawn)
{
    int n = s->autostart_count;
    s->autostart_cmds = realloc(s->autostart_cmds,
                                (n + 1) * sizeof(char *));
    s->autostart_respawn = realloc(s->autostart_respawn,
                                   (n + 1) * sizeof(int));
    s->autostart_cmds[n] = strdup(cmd);
    s->autostart_respawn[n] = respawn;
    s->autostart_count = n + 1;
}

int autostart_load(Session *s, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline/whitespace */
        char *end = line + strlen(line);
        while (end > line && isspace((unsigned char)end[-1]))
            *--end = '\0';

        /* Skip empty lines and comments */
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#')
            continue;

        /* Check for @ prefix (respawn) */
        int respawn = 0;
        if (*p == '@') {
            respawn = 1;
            p++;
            while (*p && isspace((unsigned char)*p)) p++;
        }

        if (*p)
            add_entry(s, p, respawn);
    }

    fclose(fp);
    return 0;
}

void autostart_load_xdg(Session *s)
{
    /* Read .desktop files from $XDG_CONFIG_HOME/autostart/
     * and each $XDG_CONFIG_DIRS/autostart/ */
    const char *home = isde_xdg_config_home();
    char path[512];

    snprintf(path, sizeof(path), "%s/autostart", home);
    int count = 0;
    IsdeDesktopEntry **entries = isde_desktop_scan_dir(path, &count);
    if (entries) {
        for (int i = 0; i < count; i++) {
            if (isde_desktop_should_show(entries[i], "ISDE")) {
                const char *exec = isde_desktop_exec(entries[i]);
                if (exec)
                    add_entry(s, exec, 0);
            }
            isde_desktop_free(entries[i]);
        }
        free(entries);
    }

    /* Walk XDG_CONFIG_DIRS */
    const char *dirs = isde_xdg_config_dirs();
    const char *p = dirs;
    while (p && *p) {
        const char *colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        if (dlen > 0 && dlen < sizeof(path) - 12) {
            snprintf(path, sizeof(path), "%.*s/autostart", (int)dlen, p);
            count = 0;
            entries = isde_desktop_scan_dir(path, &count);
            if (entries) {
                for (int i = 0; i < count; i++) {
                    if (isde_desktop_should_show(entries[i], "ISDE")) {
                        const char *exec = isde_desktop_exec(entries[i]);
                        if (exec)
                            add_entry(s, exec, 0);
                    }
                    isde_desktop_free(entries[i]);
                }
                free(entries);
            }
        }
        p = colon ? colon + 1 : NULL;
    }
}

void autostart_free(Session *s)
{
    for (int i = 0; i < s->autostart_count; i++)
        free(s->autostart_cmds[i]);
    free(s->autostart_cmds);
    free(s->autostart_respawn);
    s->autostart_cmds = NULL;
    s->autostart_respawn = NULL;
    s->autostart_count = 0;
}
