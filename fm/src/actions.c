/*
 * actions.c — custom script actions for the context menu
 *
 * Scans ~/.config/isde/fm-actions/ and system data dirs for executable
 * scripts with optional .desktop-style companion files that control
 * when the action appears.
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fnmatch.h>

/* Parse a semicolon-delimited value into a NULL-terminated string array. */
static char **parse_list(const char *value)
{
    if (!value || !value[0])
        return NULL;

    int cap = 4, count = 0;
    char **list = malloc(cap * sizeof(char *));
    const char *p = value;

    while (*p) {
        while (*p == ';' || *p == ' ')
            p++;
        if (!*p)
            break;
        const char *end = strchr(p, ';');
        int len = end ? (int)(end - p) : (int)strlen(p);
        while (len > 0 && p[len - 1] == ' ')
            len--;
        if (len > 0) {
            if (count + 1 >= cap) {
                cap *= 2;
                list = realloc(list, cap * sizeof(char *));
            }
            list[count++] = strndup(p, len);
        }
        p += (end ? (end - p) + 1 : len);
    }

    if (count == 0) {
        free(list);
        return NULL;
    }
    list[count] = NULL;
    return list;
}

static void free_list(char **list)
{
    if (!list) return;
    for (int i = 0; list[i]; i++)
        free(list[i]);
    free(list);
}

/* Strip file extension from basename to produce a display name. */
static char *name_from_basename(const char *basename)
{
    char *name = strdup(basename);
    char *dot = strrchr(name, '.');
    if (dot && dot != name)
        *dot = '\0';
    return name;
}

/* Parse a .desktop-style companion file for Name, MimeType, FilePattern. */
static void parse_companion(const char *path, FmAction *action)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        line[strcspn(line, "\r\n")] = '\0';

        if (strncmp(line, "Name=", 5) == 0) {
            free(action->name);
            action->name = strdup(line + 5);
        } else if (strncmp(line, "MimeType=", 9) == 0) {
            free_list(action->mime_types);
            action->mime_types = parse_list(line + 9);
        } else if (strncmp(line, "FilePattern=", 12) == 0) {
            free_list(action->file_patterns);
            action->file_patterns = parse_list(line + 12);
        }
    }
    fclose(f);
}

/* Check if an action with the same basename already exists (user overrides system). */
static int action_exists(FmApp *app, const char *basename)
{
    for (int i = 0; i < app->nactions; i++) {
        const char *existing = strrchr(app->actions[i].script_path, '/');
        existing = existing ? existing + 1 : app->actions[i].script_path;
        if (strcmp(existing, basename) == 0)
            return 1;
    }
    return 0;
}

static void scan_dir(FmApp *app, const char *dirpath, int check_dup)
{
    DIR *d = opendir(dirpath);
    if (!d)
        return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        /* Skip .desktop companion files — they're not scripts */
        size_t nlen = strlen(ent->d_name);
        if (nlen > 8 && strcmp(ent->d_name + nlen - 8, ".desktop") == 0)
            continue;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dirpath, ent->d_name);

        if (access(path, X_OK) != 0)
            continue;

        if (check_dup && action_exists(app, ent->d_name))
            continue;

        FmAction action = {0};
        action.script_path = strdup(path);
        action.name = name_from_basename(ent->d_name);

        /* Look for companion .desktop file */
        char companion[PATH_MAX];
        /* Try <basename_without_ext>.desktop */
        char *dot = strrchr(ent->d_name, '.');
        if (dot && dot != ent->d_name) {
            int base_len = (int)(dot - ent->d_name);
            snprintf(companion, sizeof(companion), "%s/%.*s.desktop",
                     dirpath, base_len, ent->d_name);
        } else {
            snprintf(companion, sizeof(companion), "%s/%s.desktop",
                     dirpath, ent->d_name);
        }
        parse_companion(companion, &action);

        int idx = app->nactions++;
        app->actions = realloc(app->actions, app->nactions * sizeof(FmAction));
        app->actions[idx] = action;
    }
    closedir(d);
}

void actions_scan(FmApp *app)
{
    app->actions = NULL;
    app->nactions = 0;

    /* User actions first (higher priority) */
    char *user_dir = isde_xdg_config_path("fm-actions");
    if (user_dir) {
        scan_dir(app, user_dir, 0);
        free(user_dir);
    }

    /* System actions from XDG data dirs */
    const char *data_dirs = isde_xdg_data_dirs();
    if (data_dirs) {
        char *dirs = strdup(data_dirs);
        char *saveptr = NULL;
        for (char *tok = strtok_r(dirs, ":", &saveptr);
             tok; tok = strtok_r(NULL, ":", &saveptr)) {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/isde/fm-actions", tok);
            scan_dir(app, path, 1);
        }
        free(dirs);
    }
}

int actions_match(FmApp *app, const FmEntry **entries, int nentries,
                  int *indices, int max)
{
    int count = 0;

    for (int a = 0; a < app->nactions && count < max; a++) {
        FmAction *act = &app->actions[a];

        int has_filter = act->mime_types || act->file_patterns;

        /* No filters → always matches */
        if (!has_filter) {
            indices[count++] = a;
            continue;
        }

        /* Filtered actions need files to check against */
        if (nentries == 0)
            continue;

        /* Action must match ALL selected entries */
        int all_match = 1;
        for (int e = 0; e < nentries; e++) {
            const FmEntry *entry = entries[e];
            int matched = 0;

            if (act->mime_types) {
                const char *mime = isde_mime_type_for_file(entry->name);
                if (mime) {
                    for (int m = 0; act->mime_types[m]; m++) {
                        if (strcmp(mime, act->mime_types[m]) == 0) {
                            matched = 1;
                            break;
                        }
                    }
                }
            }

            if (!matched && act->file_patterns) {
                for (int p = 0; act->file_patterns[p]; p++) {
                    if (fnmatch(act->file_patterns[p], entry->name, 0) == 0) {
                        matched = 1;
                        break;
                    }
                }
            }

            if (!matched) {
                all_match = 0;
                break;
            }
        }

        if (all_match)
            indices[count++] = a;
    }
    return count;
}

void actions_cleanup(FmApp *app)
{
    for (int i = 0; i < app->nactions; i++) {
        free(app->actions[i].script_path);
        free(app->actions[i].name);
        free_list(app->actions[i].mime_types);
        free_list(app->actions[i].file_patterns);
    }
    free(app->actions);
    app->actions = NULL;
    app->nactions = 0;
}
