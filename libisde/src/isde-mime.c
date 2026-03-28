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
    if (glob_loaded) return;
    glob_loaded = 1;

    FILE *fp = fopen("/usr/share/mime/globs2", "r");
    if (!fp) return;

    int cap = 512;
    glob_table = malloc(cap * sizeof(GlobEntry));
    glob_count = 0;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#') continue;

        /* Format: weight:mime:pattern */
        char *p1 = strchr(line, ':');
        if (!p1) continue;
        char *p2 = strchr(p1 + 1, ':');
        if (!p2) continue;

        /* Only handle *.ext patterns */
        char *pattern = p2 + 1;
        /* Strip trailing newline */
        char *nl = strchr(pattern, '\n');
        if (nl) *nl = '\0';

        if (pattern[0] != '*' || pattern[1] != '.')
            continue;
        /* Skip patterns with wildcards in the extension */
        if (strchr(pattern + 2, '*') || strchr(pattern + 2, '?'))
            continue;

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

    if (!filename) return "application/octet-stream";
    const char *dot = strrchr(filename, '.');
    if (!dot || !dot[1]) return "application/octet-stream";
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
