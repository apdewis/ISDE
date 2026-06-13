#define _POSIX_C_SOURCE 200809L
/*
 * isde-desktop.c — freedesktop.org .desktop file parser
 *
 * Parses the [Desktop Entry] group per the Desktop Entry Specification.
 * This is a simple line-based parser (not TOML) since .desktop files
 * use their own INI-like format defined by freedesktop.org.
 */
#include "desktop.h"
#include "ewmh.h"
#include "../../common/isde-config.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char *id;
    char *name;
    char *exec;
    char *icon;
} DesktopAction;

struct IsdeDesktopEntry {
    char *desktop_id;    /* basename of .desktop file (e.g. "firefox.desktop") */
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
    int   startup_notify;
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
    } else if (strcmp(key, "StartupNotify") == 0) {
        e->startup_notify = (strcmp(val, "true") == 0);
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

    const char *slash = strrchr(path, '/');
    e->desktop_id = strdup(slash ? slash + 1 : path);

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
    free(e->desktop_id);
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

const char *isde_desktop_id(const IsdeDesktopEntry *e)           { return e->desktop_id; }
const char *isde_desktop_name(const IsdeDesktopEntry *e)         { return e->name; }
const char *isde_desktop_generic_name(const IsdeDesktopEntry *e) { return e->generic_name; }
const char *isde_desktop_comment(const IsdeDesktopEntry *e)      { return e->comment; }
const char *isde_desktop_exec(const IsdeDesktopEntry *e)         { return e->exec; }
const char *isde_desktop_icon(const IsdeDesktopEntry *e)         { return e->icon; }
const char *isde_desktop_type(const IsdeDesktopEntry *e)         { return e->type; }
const char *isde_desktop_categories(const IsdeDesktopEntry *e)   { return e->categories; }
const char *isde_desktop_mime_types(const IsdeDesktopEntry *e)   { return e->mime_types; }
const char *isde_desktop_startup_wm_class(const IsdeDesktopEntry *e) { return e->startup_wm_class; }
int         isde_desktop_startup_notify(const IsdeDesktopEntry *e) { return e->startup_notify; }
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

/* ---------- startup notification helpers ---------- */

static long startup_seq;

static char *generate_startup_id(void)
{
    char hostname[64];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        snprintf(hostname, sizeof(hostname), "localhost");
    }
    hostname[sizeof(hostname) - 1] = '\0';

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long seq = ++startup_seq;

    char buf[256];
    snprintf(buf, sizeof(buf), "%s+%d-%ld-%ld_TIME%lu",
             hostname, (int)getpid(), seq,
             (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000,
             (unsigned long)time(NULL));
    return strdup(buf);
}

static void send_startup_message(IsdeEwmh *ewmh, const char *msg)
{
    xcb_connection_t *conn = isde_ewmh_xcb(ewmh);
    xcb_window_t root = isde_ewmh_root(ewmh);
    xcb_atom_t begin_atom = isde_ewmh_atom_startup_info_begin(ewmh);
    xcb_atom_t cont_atom = isde_ewmh_atom_startup_info(ewmh);

    size_t len = strlen(msg) + 1;
    const char *p = msg;
    int first = 1;

    while (len > 0) {
        xcb_client_message_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.window = root;
        ev.type = first ? begin_atom : cont_atom;
        ev.format = 8;

        size_t chunk = len > 20 ? 20 : len;
        memcpy(ev.data.data8, p, chunk);

        xcb_send_event(conn, 0, root,
                       XCB_EVENT_MASK_PROPERTY_CHANGE,
                       (const char *)&ev);
        p += chunk;
        len -= chunk;
        first = 0;
    }
    xcb_flush(conn);
}

static char *quote_value(const char *val)
{
    if (!val) {
        return strdup("\"\"");
    }

    int need_quote = 0;
    for (const char *p = val; *p; p++) {
        if (*p == ' ' || *p == '"' || *p == '\\') {
            need_quote = 1;
            break;
        }
    }

    if (!need_quote) {
        return strdup(val);
    }

    size_t len = strlen(val);
    char *out = malloc(len * 2 + 3);
    char *o = out;
    *o++ = '"';
    for (const char *p = val; *p; p++) {
        if (*p == '"' || *p == '\\') {
            *o++ = '\\';
        }
        *o++ = *p;
    }
    *o++ = '"';
    *o = '\0';
    return out;
}

static void send_startup_new(IsdeEwmh *ewmh, const char *id,
                             const IsdeDesktopEntry *e)
{
    char *qname = quote_value(e->name);
    char *qbin = quote_value(e->exec);
    char *qwmclass = quote_value(e->startup_wm_class);

    int screen = 0;
    xcb_screen_t *scr = isde_ewmh_screen(ewmh);
    xcb_connection_t *conn = isde_ewmh_xcb(ewmh);
    xcb_screen_iterator_t si = xcb_setup_roots_iterator(xcb_get_setup(conn));
    for (int i = 0; si.rem; xcb_screen_next(&si), i++) {
        if (si.data == scr) {
            screen = i;
            break;
        }
    }

    char msg[1024];
    int pos = snprintf(msg, sizeof(msg),
                       "new: ID=%s NAME=%s SCREEN=%d",
                       id, qname, screen);
    if (e->exec) {
        pos += snprintf(msg + pos, sizeof(msg) - pos, " BIN=%s", qbin);
    }
    if (e->startup_wm_class) {
        pos += snprintf(msg + pos, sizeof(msg) - pos, " WMCLASS=%s", qwmclass);
    }
    pos += snprintf(msg + pos, sizeof(msg) - pos, " TIMESTAMP=%lu",
                    (unsigned long)time(NULL));

    free(qname);
    free(qbin);
    free(qwmclass);

    send_startup_message(ewmh, msg);
}

void isde_desktop_startup_remove(IsdeEwmh *ewmh, const char *id)
{
    char msg[256];
    snprintf(msg, sizeof(msg), "remove: ID=%s", id);
    send_startup_message(ewmh, msg);
}

static pid_t launch_cmd_with_id(const char *cmd, const char *startup_id)
{
    if (!cmd) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        if (startup_id) {
            setenv("DESKTOP_STARTUP_ID", startup_id, 1);
        }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static pid_t launch_terminal_with_id(const char *cmd, const char *startup_id)
{
    if (!cmd) return -1;
    const char *term = isde_desktop_get_terminal();
    pid_t pid = fork();
    if (pid == 0) {
        if (startup_id) {
            setenv("DESKTOP_STARTUP_ID", startup_id, 1);
        }
        execlp(term, term, "-e", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    return pid;
}

pid_t isde_desktop_launch_cmd(const char *cmd)
{
    return launch_cmd_with_id(cmd, NULL);
}

const char *isde_desktop_get_terminal(void)
{
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *session = isde_config_table(root, "session");
        if (session) {
            const char *term = isde_config_string(session, "terminal", NULL);
            if (term) {
                static char buf[256];
                snprintf(buf, sizeof(buf), "%s", term);
                isde_config_free(cfg);
                return buf;
            }
        }
        isde_config_free(cfg);
    }
    return "isde-term";
}

pid_t isde_desktop_launch_in_terminal(const char *cmd)
{
    return launch_terminal_with_id(cmd, NULL);
}

pid_t isde_desktop_launch(const IsdeDesktopEntry *e,
                          const char **files, int nfiles)
{
    char *cmd = isde_desktop_build_exec(e, files, nfiles);
    if (!cmd) return -1;

    pid_t pid;
    if (isde_desktop_terminal(e)) {
        pid = launch_terminal_with_id(cmd, NULL);
    } else {
        pid = launch_cmd_with_id(cmd, NULL);
    }
    free(cmd);
    return pid;
}

pid_t isde_desktop_launch_notify(const IsdeDesktopEntry *e,
                                 const char **files, int nfiles,
                                 IsdeEwmh *ewmh, char **id_out)
{
    if (id_out) {
        *id_out = NULL;
    }

    char *cmd = isde_desktop_build_exec(e, files, nfiles);
    if (!cmd) return -1;

    char *startup_id = NULL;
    if (ewmh && e->startup_notify) {
        startup_id = generate_startup_id();
        send_startup_new(ewmh, startup_id, e);
    }

    pid_t pid;
    if (isde_desktop_terminal(e)) {
        pid = launch_terminal_with_id(cmd, startup_id);
    } else {
        pid = launch_cmd_with_id(cmd, startup_id);
    }
    free(cmd);

    if (pid < 0 && startup_id && ewmh) {
        isde_desktop_startup_remove(ewmh, startup_id);
        free(startup_id);
    } else if (id_out && startup_id) {
        *id_out = startup_id;
    } else {
        free(startup_id);
    }

    return pid;
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