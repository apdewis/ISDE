#define _POSIX_C_SOURCE 200809L
/*
 * isde-desktop.c — freedesktop.org .desktop file parser
 *
 * Parses the [Desktop Entry] group per the Desktop Entry Specification.
 * This is a simple line-based parser (not TOML) since .desktop files
 * use their own INI-like format defined by freedesktop.org.
 */
#include "isde/isde-desktop.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *id;
    char *name;
    char *exec;
    char *icon;
} DesktopAction;

struct IsdeDesktopEntry {
    char *name;
    char *generic_name;
    char *comment;
    char *exec;
    char *icon;
    char *type;
    char *categories;
    char *mime_types;
    char *startup_wm_class;
    char *only_show_in;
    char *not_show_in;
    char *actions_str;   /* raw "Actions=" value (semicolon-separated IDs) */
    DesktopAction *actions;
    int   nactions;
    int   terminal;
    int   no_display;
    int   hidden;
};

static char *strip(char *s)
{
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return s;
}

static void set_field(IsdeDesktopEntry *e, const char *key, const char *val)
{
    if (strcmp(key, "Name") == 0) {
        e->name = strdup(val);
    } else if (strcmp(key, "GenericName") == 0) {
        e->generic_name = strdup(val);
    } else if (strcmp(key, "Comment") == 0) {
        e->comment = strdup(val);
    } else if (strcmp(key, "Exec") == 0) {
        e->exec = strdup(val);
    } else if (strcmp(key, "Icon") == 0) {
        e->icon = strdup(val);
    } else if (strcmp(key, "Type") == 0) {
        e->type = strdup(val);
    } else if (strcmp(key, "Categories") == 0) {
        e->categories = strdup(val);
    } else if (strcmp(key, "MimeType") == 0) {
        e->mime_types = strdup(val);
    } else if (strcmp(key, "OnlyShowIn") == 0) {
        e->only_show_in = strdup(val);
    } else if (strcmp(key, "NotShowIn") == 0) {
        e->not_show_in = strdup(val);
    } else if (strcmp(key, "Terminal") == 0) {
        e->terminal = (strcmp(val, "true") == 0);
    } else if (strcmp(key, "NoDisplay") == 0) {
        e->no_display = (strcmp(val, "true") == 0);
    } else if (strcmp(key, "Hidden") == 0) {
        e->hidden = (strcmp(val, "true") == 0);
    } else if (strcmp(key, "StartupWMClass") == 0) {
        e->startup_wm_class = strdup(val);
    } else if (strcmp(key, "Actions") == 0) {
        e->actions_str = strdup(val);
    }
}

/* Find an action by ID in the pre-allocated actions array */
static DesktopAction *find_action(IsdeDesktopEntry *e, const char *id)
{
    for (int i = 0; i < e->nactions; i++) {
        if (e->actions[i].id && strcmp(e->actions[i].id, id) == 0) {
            return &e->actions[i];
        }
    }
    return NULL;
}

/* Parse the Actions= string and allocate action slots */
static void parse_action_ids(IsdeDesktopEntry *e)
{
    if (!e->actions_str) { return; }

    /* Count semicolons to estimate action count */
    int count = 0;
    const char *p = e->actions_str;
    while (*p) {
        if (*p == ';') {
            count++;
        } else if (!count || p[-1] == ';') {
            count++;
        }
        p++;
    }

    e->actions = calloc(count + 1, sizeof(DesktopAction));
    e->nactions = 0;

    /* Split by semicolons */
    char *tmp = strdup(e->actions_str);
    char *tok = strtok(tmp, ";");
    while (tok) {
        char *id = strip(tok);
        if (*id) {
            e->actions[e->nactions].id = strdup(id);
            e->nactions++;
        }
        tok = strtok(NULL, ";");
    }
    free(tmp);
}

IsdeDesktopEntry *isde_desktop_load(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return NULL;
    }

    IsdeDesktopEntry *e = calloc(1, sizeof(*e));
    if (!e) {
        fclose(fp);
        return NULL;
    }

    char line[1024];
    int in_desktop_entry = 0;
    char *current_action_id = NULL;  /* non-NULL when inside [Desktop Action X] */

    while (fgets(line, sizeof(line), fp)) {
        char *s = strip(line);
        if (!*s || *s == '#') {
            continue;
        }

        /* Group header */
        if (*s == '[') {
            in_desktop_entry = 0;
            free(current_action_id);
            current_action_id = NULL;

            if (strncmp(s, "[Desktop Entry]", 15) == 0) {
                in_desktop_entry = 1;
            } else if (strncmp(s, "[Desktop Action ", 16) == 0) {
                /* Extract action ID from "[Desktop Action foo]" */
                char *end = strchr(s + 16, ']');
                if (end) {
                    *end = '\0';
                    current_action_id = strdup(s + 16);
                }
            }
            continue;
        }

        /* Key=Value — skip localized keys (contain '[') */
        char *eq = strchr(s, '=');
        if (!eq) {
            continue;
        }
        if (memchr(s, '[', eq - s)) {
            continue;
        }

        *eq = '\0';
        char *key = strip(s);
        char *val = strip(eq + 1);

        if (in_desktop_entry) {
            set_field(e, key, val);
        } else if (current_action_id) {
            /* We're inside a [Desktop Action] group.
             * Actions need to be allocated first, so store raw
             * and do a second pass — or allocate lazily. */
            if (!e->actions) {
                /* First action group encountered before Actions= parsed.
                 * Parse action IDs now if we have the string. */
                parse_action_ids(e);
            }
            DesktopAction *a = find_action(e, current_action_id);
            if (a) {
                if (strcmp(key, "Name") == 0) {
                    a->name = strdup(val);
                } else if (strcmp(key, "Exec") == 0) {
                    a->exec = strdup(val);
                } else if (strcmp(key, "Icon") == 0) {
                    a->icon = strdup(val);
                }
            }
        }
    }

    free(current_action_id);
    fclose(fp);

    /* Ensure actions are parsed even if [Desktop Action] groups
     * appeared before the Actions= key (unlikely but possible) */
    if (!e->actions && e->actions_str) {
        parse_action_ids(e);
    }

    return e;
}

void isde_desktop_free(IsdeDesktopEntry *e)
{
    if (!e) {
        return;
    }
    free(e->name);
    free(e->generic_name);
    free(e->comment);
    free(e->exec);
    free(e->icon);
    free(e->type);
    free(e->categories);
    free(e->mime_types);
    free(e->startup_wm_class);
    free(e->only_show_in);
    free(e->not_show_in);
    free(e->actions_str);
    for (int i = 0; i < e->nactions; i++) {
        free(e->actions[i].id);
        free(e->actions[i].name);
        free(e->actions[i].exec);
        free(e->actions[i].icon);
    }
    free(e->actions);
    free(e);
}

int isde_desktop_action_count(const IsdeDesktopEntry *e)
{
    return e ? e->nactions : 0;
}

const IsdeDesktopAction *isde_desktop_action(const IsdeDesktopEntry *e,
                                              int index)
{
    if (!e || index < 0 || index >= e->nactions) {
        return NULL;
    }
    DesktopAction *a = &e->actions[index];
    /* Return a pointer that matches the public struct layout —
     * DesktopAction and IsdeDesktopAction have the same fields */
    return (const IsdeDesktopAction *)a;
}

const char *isde_desktop_name(const IsdeDesktopEntry *e)         { return e->name; }
const char *isde_desktop_generic_name(const IsdeDesktopEntry *e) { return e->generic_name; }
const char *isde_desktop_comment(const IsdeDesktopEntry *e)      { return e->comment; }
const char *isde_desktop_exec(const IsdeDesktopEntry *e)         { return e->exec; }
const char *isde_desktop_icon(const IsdeDesktopEntry *e)         { return e->icon; }
const char *isde_desktop_type(const IsdeDesktopEntry *e)         { return e->type; }
const char *isde_desktop_categories(const IsdeDesktopEntry *e)   { return e->categories; }
const char *isde_desktop_mime_types(const IsdeDesktopEntry *e)   { return e->mime_types; }
const char *isde_desktop_startup_wm_class(const IsdeDesktopEntry *e) { return e->startup_wm_class; }
int         isde_desktop_terminal(const IsdeDesktopEntry *e)     { return e->terminal; }
int         isde_desktop_no_display(const IsdeDesktopEntry *e)   { return e->no_display; }
int         isde_desktop_hidden(const IsdeDesktopEntry *e)       { return e->hidden; }

/* Check if a semicolon-separated list contains an entry */
static int list_contains(const char *list, const char *item)
{
    if (!list) {
        return 0;
    }
    size_t ilen = strlen(item);
    const char *p = list;
    while (*p) {
        const char *semi = strchr(p, ';');
        size_t elen = semi ? (size_t)(semi - p) : strlen(p);
        if (elen == ilen && strncmp(p, item, ilen) == 0) {
            return 1;
        }
        p = semi ? semi + 1 : p + elen;
    }
    return 0;
}

int isde_desktop_should_show(const IsdeDesktopEntry *e, const char *desktop)
{
    if (e->hidden || e->no_display) {
        return 0;
    }
    if (e->only_show_in) {
        return list_contains(e->only_show_in, desktop);
    }
    if (e->not_show_in) {
        return !list_contains(e->not_show_in, desktop);
    }
    return 1;
}

int isde_desktop_handles_mime(const IsdeDesktopEntry *e, const char *mime)
{
    if (!e || !e->mime_types || !mime) {
        return 0;
    }
    return list_contains(e->mime_types, mime);
}

char *isde_desktop_build_exec(const IsdeDesktopEntry *e,
                              const char **files, int nfiles)
{
    if (!e || !e->exec) {
        return NULL;
    }

    const char *src = e->exec;
    /* Estimate output size */
    size_t cap = strlen(src) + 256;
    for (int i = 0; i < nfiles; i++) {
        cap += strlen(files[i]) + 3; /* space + quotes */
    }

    char *out = malloc(cap);
    if (!out) {
        return NULL;
    }

    size_t pos = 0;
    while (*src) {
        if (*src == '%' && src[1]) {
            src++;
            switch (*src) {
            case 'f': case 'u':
                /* Single file/URL */
                if (nfiles > 0) {
                    pos += snprintf(out + pos, cap - pos, "'%s'", files[0]);
                }
                break;
            case 'F': case 'U':
                /* Multiple files/URLs */
                for (int i = 0; i < nfiles; i++) {
                    pos += snprintf(out + pos, cap - pos, "%s'%s'",
                                    i ? " " : "", files[i]);
                }
                break;
            case 'i':
                if (e->icon) {
                    pos += snprintf(out + pos, cap - pos, "--icon '%s'",
                                    e->icon);
                }
                break;
            case 'c':
                if (e->name) {
                    pos += snprintf(out + pos, cap - pos, "'%s'", e->name);
                }
                break;
            case 'k':
                /* Desktop file path — not available here, skip */
                break;
            case '%':
                if (pos < cap - 1) {
                    out[pos++] = '%';
                }
                break;
            default:
                /* Unknown code, skip */
                break;
            }
            src++;
        } else {
            if (pos < cap - 1) {
                out[pos++] = *src;
            }
            src++;
        }
    }
    out[pos] = '\0';
    return out;
}

IsdeDesktopEntry **isde_desktop_scan_dir(const char *dirpath, int *count)
{
    *count = 0;
    DIR *dir = opendir(dirpath);
    if (!dir) {
        return NULL;
    }

    int cap = 64;
    IsdeDesktopEntry **entries = calloc(cap, sizeof(*entries));
    if (!entries) {
        closedir(dir);
        return NULL;
    }

    struct dirent *de;
    while ((de = readdir(dir))) {
        size_t nlen = strlen(de->d_name);
        if (nlen < 9 || strcmp(de->d_name + nlen - 8, ".desktop") != 0) {
            continue;
        }

        size_t plen = strlen(dirpath) + 1 + nlen + 1;
        char *path = malloc(plen);
        if (!path) {
            continue;
        }
        snprintf(path, plen, "%s/%s", dirpath, de->d_name);

        IsdeDesktopEntry *e = isde_desktop_load(path);
        free(path);
        if (!e) {
            continue;
        }

        if (*count >= cap) {
            cap *= 2;
            IsdeDesktopEntry **tmp = realloc(entries, cap * sizeof(*entries));
            if (!tmp) {
                isde_desktop_free(e);
                continue;
            }
            entries = tmp;
        }
        entries[(*count)++] = e;
    }

    closedir(dir);
    return entries;
}
