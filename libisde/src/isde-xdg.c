/*
 * isde-xdg.c — XDG Base Directory Specification helpers
 */
#include "isde/isde-xdg.h"
#include "isde/isde-config.h"
#include "isde/isde-theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *get_home(void)
{
    const char *h = getenv("HOME");
    return h ? h : "/tmp";
}

const char *isde_xdg_config_home(void)
{
    const char *v = getenv("XDG_CONFIG_HOME");
    if (v && v[0])
        return v;
    static char buf[512];
    if (!buf[0])
        snprintf(buf, sizeof(buf), "%s/.config", get_home());
    return buf;
}

const char *isde_xdg_data_home(void)
{
    const char *v = getenv("XDG_DATA_HOME");
    if (v && v[0])
        return v;
    static char buf[512];
    if (!buf[0])
        snprintf(buf, sizeof(buf), "%s/.local/share", get_home());
    return buf;
}

const char *isde_xdg_cache_home(void)
{
    const char *v = getenv("XDG_CACHE_HOME");
    if (v && v[0])
        return v;
    static char buf[512];
    if (!buf[0])
        snprintf(buf, sizeof(buf), "%s/.cache", get_home());
    return buf;
}

const char *isde_xdg_config_dirs(void)
{
    const char *v = getenv("XDG_CONFIG_DIRS");
    return (v && v[0]) ? v : "/etc/xdg";
}

const char *isde_xdg_data_dirs(void)
{
    const char *v = getenv("XDG_DATA_DIRS");
    return (v && v[0]) ? v : "/usr/local/share:/usr/share";
}

char *isde_xdg_config_path(const char *suffix)
{
    const char *base = isde_xdg_config_home();
    size_t len = strlen(base) + strlen("/isde/") + strlen(suffix) + 1;
    char *path = malloc(len);
    if (path)
        snprintf(path, len, "%s/isde/%s", base, suffix);
    return path;
}

char *isde_xdg_data_path(const char *suffix)
{
    const char *base = isde_xdg_data_home();
    size_t len = strlen(base) + strlen("/isde/") + strlen(suffix) + 1;
    char *path = malloc(len);
    if (path)
        snprintf(path, len, "%s/isde/%s", base, suffix);
    return path;
}

/* Search a colon-separated list of directories for isde/<name> */
static char *find_in_dirs(const char *home, const char *dirs, const char *name)
{
    /* Check home first */
    size_t hlen = strlen(home) + strlen("/isde/") + strlen(name) + 1;
    char *path = malloc(hlen);
    if (!path)
        return NULL;
    snprintf(path, hlen, "%s/isde/%s", home, name);
    if (access(path, R_OK) == 0)
        return path;
    free(path);

    /* Walk colon-separated dirs */
    const char *p = dirs;
    while (p && *p) {
        const char *colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        if (dlen > 0) {
            size_t total = dlen + strlen("/isde/") + strlen(name) + 1;
            path = malloc(total);
            if (!path)
                return NULL;
            snprintf(path, total, "%.*s/isde/%s", (int)dlen, p, name);
            if (access(path, R_OK) == 0)
                return path;
            free(path);
        }
        p = colon ? colon + 1 : NULL;
    }
    return NULL;
}

char *isde_xdg_find_config(const char *name)
{
    return find_in_dirs(isde_xdg_config_home(), isde_xdg_config_dirs(), name);
}

char *isde_xdg_find_data(const char *name)
{
    return find_in_dirs(isde_xdg_data_home(), isde_xdg_data_dirs(), name);
}

char *isde_icon_find(const char *category, const char *name)
{
    /* Try configured icon theme, falling back to hicolor */
    char theme_buf[128] = "hicolor";
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *appear = isde_config_table(root, "appearance");
        if (appear) {
            const char *theme = isde_config_string(appear, "icon_theme", NULL);
            if (theme) {
                snprintf(theme_buf, sizeof(theme_buf), "%s", theme);
            }
        }
        isde_config_free(cfg);
    }
    char *path = isde_icon_theme_lookup(theme_buf, category, name);
    if (path) {
        return path;
    }

    /* Fallback: ISDE bundled icons in isde/icons/<category>/<name>.svg */
    char rel[256];
    snprintf(rel, sizeof(rel), "icons/%s/%s.svg", category, name);

    char *path = isde_xdg_find_data(rel);
    if (path)
        return path;

    /* Dev build fallback: check relative to executable */
    static char exe_dir[512] = {0};
    if (!exe_dir[0]) {
        ssize_t len = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
        if (len > 0) {
            exe_dir[len] = '\0';
            char *slash = strrchr(exe_dir, '/');
            if (slash) *slash = '\0';
        }
    }
    if (exe_dir[0]) {
        size_t total = strlen(exe_dir) + strlen("/../../common/data/") +
                       strlen(rel) + 1;
        path = malloc(total);
        if (path) {
            snprintf(path, total, "%s/../../common/data/%s", exe_dir, rel);
            if (access(path, R_OK) == 0)
                return path;
            free(path);
        }
    }

    return NULL;
}

char *isde_xdg_user_dir(const char *name)
{
    /* Build path to user-dirs.dirs */
    char path[512];
    snprintf(path, sizeof(path), "%s/user-dirs.dirs", isde_xdg_config_home());

    FILE *fp = fopen(path, "r");
    if (!fp)
        return NULL;

    /* Build the key we're looking for: XDG_xxx_DIR */
    char key[64];
    snprintf(key, sizeof(key), "XDG_%s_DIR", name);
    size_t klen = strlen(key);

    char line[1024];
    char *result = NULL;
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    while (fgets(line, sizeof(line), fp)) {
        /* Skip comments and blank lines */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0')
            continue;

        /* Match key= */
        if (strncmp(p, key, klen) != 0 || p[klen] != '=')
            continue;

        p += klen + 1;
        /* Skip whitespace after = */
        while (*p == ' ' || *p == '\t') p++;

        /* Value is quoted: "..." */
        if (*p != '"')
            continue;
        p++;

        /* Find closing quote */
        char *end = strchr(p, '"');
        if (!end)
            continue;
        *end = '\0';

        /* Expand $HOME */
        if (strncmp(p, "$HOME/", 6) == 0) {
            size_t total = strlen(home) + strlen(p + 5) + 1;
            result = malloc(total);
            if (result)
                snprintf(result, total, "%s%s", home, p + 5);
        } else if (strcmp(p, "$HOME") == 0) {
            /* Set to $HOME means "not configured" per spec */
            result = NULL;
        } else if (p[0] == '/') {
            result = strdup(p);
        }
        break;
    }

    fclose(fp);
    return result;
}

int isde_scale_percent(void)
{
    const char *env = getenv("ISW_SCALE_FACTOR");
    if (env) {
        int v = (int)(atof(env) * 100 + 0.5);
        return v > 0 ? v : 100;
    }
    return 100;
}

int isde_scale(int value)
{
    return value * isde_scale_percent() / 100;
}
