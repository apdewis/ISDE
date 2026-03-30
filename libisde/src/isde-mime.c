#define _POSIX_C_SOURCE 200809L
/*
 * isde-mime.c — MIME type detection from file extensions
 *
 * Parses /usr/share/mime/globs2 (shared-mime-info format):
 *   weight:mime-type:glob-pattern
 * Only simple *.ext patterns are matched.
 */
#include "isde/isde-mime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

typedef struct {
    char *ext;       /* lowercase extension without dot */
    char *mime;      /* MIME type string */
    int   weight;
} GlobEntry;

static GlobEntry *glob_table;
static int         glob_count;
static int         glob_loaded;

static void load_globs(void)
{
    if (glob_loaded) { return; }
    glob_loaded = 1;

    FILE *fp = fopen("/usr/share/mime/globs2", "r");
    if (!fp) { return; }

    int cap = 512;
    glob_table = malloc(cap * sizeof(GlobEntry));
    glob_count = 0;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#') { continue; }

        /* Format: weight:mime:pattern */
        char *p1 = strchr(line, ':');
        if (!p1) { continue; }
        char *p2 = strchr(p1 + 1, ':');
        if (!p2) { continue; }

        /* Only handle *.ext patterns */
        char *pattern = p2 + 1;
        /* Strip trailing newline */
        char *nl = strchr(pattern, '\n');
        if (nl) { *nl = '\0'; }

        if (pattern[0] != '*' || pattern[1] != '.') {
            continue;
        }
        /* Skip patterns with wildcards in the extension */
        if (strchr(pattern + 2, '*') || strchr(pattern + 2, '?')) {
            continue;
        }

        if (glob_count >= cap) {
            cap *= 2;
            glob_table = realloc(glob_table, cap * sizeof(GlobEntry));
        }

        *p1 = '\0';
        *p2 = '\0';

        GlobEntry *e = &glob_table[glob_count++];
        e->weight = atoi(line);
        e->mime = strdup(p1 + 1);
        e->ext = strdup(pattern + 2); /* skip "*." */
    }

    fclose(fp);
}

const char *isde_mime_type_for_file(const char *filename)
{
    load_globs();

    if (!filename) { return "application/octet-stream"; }
    const char *dot = strrchr(filename, '.');
    if (!dot || !dot[1]) { return "application/octet-stream"; }
    const char *ext = dot + 1;

    /* Find highest-weight match */
    const char *best = NULL;
    int best_weight = -1;

    for (int i = 0; i < glob_count; i++) {
        if (strcasecmp(glob_table[i].ext, ext) == 0) {
            if (glob_table[i].weight > best_weight) {
                best_weight = glob_table[i].weight;
                best = glob_table[i].mime;
            }
        }
    }

    return best ? best : "application/octet-stream";
}

/* ---------- mimeapps.list lookup ---------- */

/* Search a single mimeapps.list file for a MIME type's default app.
 * Returns malloc'd desktop ID or NULL. */
static char *search_mimeapps(const char *path, const char *mime)
{
    FILE *fp = fopen(path, "r");
    if (!fp) { return NULL; }

    size_t mlen = strlen(mime);
    int in_defaults = 0;
    char line[1024];
    char *result = NULL;

    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing whitespace */
        char *end = line + strlen(line);
        while (end > line && (end[-1] == '\n' || end[-1] == '\r' ||
                              end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }

        if (line[0] == '[') {
            in_defaults = (strcmp(line, "[Default Applications]") == 0);
            continue;
        }
        if (!in_defaults || line[0] == '#' || !line[0]) {
            continue;
        }

        /* mime=desktop.desktop;other.desktop;... */
        char *eq = strchr(line, '=');
        if (!eq) { continue; }

        if ((size_t)(eq - line) == mlen && strncmp(line, mime, mlen) == 0) {
            /* Take the first entry (before any semicolon) */
            char *val = eq + 1;
            char *semi = strchr(val, ';');
            if (semi) { *semi = '\0'; }
            /* Strip whitespace from value */
            while (*val == ' ' || *val == '\t') { val++; }
            if (*val) { result = strdup(val); }
            break;
        }
    }

    fclose(fp);
    return result;
}

char *isde_mime_default_app(const char *mime)
{
    if (!mime) { return NULL; }
    char *result;

    /* 1. User config */
    const char *home = getenv("HOME");
    if (home) {
        char path[512];
        snprintf(path, sizeof(path), "%s/.config/mimeapps.list", home);
        result = search_mimeapps(path, mime);
        if (result) { return result; }

        /* Also check XDG_CONFIG_HOME if set */
        const char *xdg_config = getenv("XDG_CONFIG_HOME");
        if (xdg_config) {
            snprintf(path, sizeof(path), "%s/mimeapps.list", xdg_config);
            result = search_mimeapps(path, mime);
            if (result) { return result; }
        }
    }

    /* 2. System applications */
    result = search_mimeapps("/usr/share/applications/mimeapps.list", mime);
    if (result) { return result; }

    result = search_mimeapps("/usr/local/share/applications/mimeapps.list",
                             mime);
    if (result) { return result; }

    /* 3. Legacy defaults.list */
    result = search_mimeapps("/usr/share/applications/defaults.list", mime);
    return result;
}

char *isde_mime_find_desktop(const char *desktop_id)
{
    if (!desktop_id) { return NULL; }

    static const char *search_dirs[] = {
        NULL, /* placeholder for ~/.local/share/applications */
        "/usr/share/applications",
        "/usr/local/share/applications",
    };

    char local_dir[512] = "";
    const char *home = getenv("HOME");
    if (home) {
        snprintf(local_dir, sizeof(local_dir),
                 "%s/.local/share/applications", home);
    }

    for (int i = 0; i < 3; i++) {
        const char *dir = (i == 0) ? local_dir : search_dirs[i];
        if (!dir[0]) { continue; }

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, desktop_id);

        if (access(path, R_OK) == 0) {
            return strdup(path);
        }
    }
    return NULL;
}

int isde_mime_set_default(const char *mime, const char *desktop_id)
{
    if (!mime || !desktop_id) { return -1; }

    const char *home = getenv("HOME");
    if (!home) { return -1; }

    char path[512];
    snprintf(path, sizeof(path), "%s/.config/mimeapps.list", home);

    /* Read existing file */
    char **lines = NULL;
    int nlines = 0;
    int cap = 0;
    int found = 0;
    int section_end = -1; /* insert point if key not found */
    int in_defaults = 0;
    int has_section = 0;

    FILE *fp = fopen(path, "r");
    if (fp) {
        char line[1024];
        while (fgets(line, sizeof(line), fp)) {
            if (nlines >= cap) {
                cap = cap ? cap * 2 : 64;
                lines = realloc(lines, cap * sizeof(char *));
            }

            /* Check if this is the key we want to update */
            if (in_defaults && !found) {
                size_t mlen = strlen(mime);

                char *eq = strchr(line, '=');
                if (eq && (size_t)(eq - line) == mlen &&
                    strncmp(line, mime, mlen) == 0) {
                    /* Replace this line */
                    char buf[1024];
                    snprintf(buf, sizeof(buf), "%s=%s\n", mime, desktop_id);
                    lines[nlines++] = strdup(buf);
                    found = 1;
                    continue;
                }
            }

            if (line[0] == '[') {
                if (in_defaults && !found) {
                    section_end = nlines; /* before this new section */
                }
                in_defaults = (strncmp(line, "[Default Applications]", 22) == 0);
                if (in_defaults) { has_section = 1; }
            }

            lines[nlines++] = strdup(line);
        }
        if (in_defaults && !found) {
            section_end = nlines;
        }
        fclose(fp);
    }

    /* Insert or append as needed */
    if (!found) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s=%s\n", mime, desktop_id);

        if (!has_section) {
            /* Append section + entry */
            if (nlines >= cap) {
                cap += 4;
                lines = realloc(lines, cap * sizeof(char *));
            }
            if (nlines > 0) { lines[nlines++] = strdup("\n"); }
            lines[nlines++] = strdup("[Default Applications]\n");
            lines[nlines++] = strdup(buf);
        } else if (section_end >= 0) {
            /* Insert at end of [Default Applications] section */
            if (nlines + 1 >= cap) {
                cap += 4;
                lines = realloc(lines, cap * sizeof(char *));
            }
            memmove(&lines[section_end + 1], &lines[section_end],
                    (nlines - section_end) * sizeof(char *));
            lines[section_end] = strdup(buf);
            nlines++;
        }
    }

    /* Write back */
    fp = fopen(path, "w");
    if (!fp) {
        for (int i = 0; i < nlines; i++) { free(lines[i]); }
        free(lines);
        return -1;
    }
    for (int i = 0; i < nlines; i++) {
        fputs(lines[i], fp);
        free(lines[i]);
    }

    free(lines);
    fclose(fp);
    return 0;
}
