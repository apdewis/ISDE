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
    if (v && v[0]) {
        return v;
    }
    static char buf[512];
    if (!buf[0]) {
        snprintf(buf, sizeof(buf), "%s/.config", get_home());
    }
    return buf;
}

const char *isde_xdg_data_home(void)
{
    const char *v = getenv("XDG_DATA_HOME");
    if (v && v[0]) {
        return v;
    }
    static char buf[512];
    if (!buf[0]) {
        snprintf(buf, sizeof(buf), "%s/.local/share", get_home());
    }
    return buf;
}

const char *isde_xdg_cache_home(void)
{
    const char *v = getenv("XDG_CACHE_HOME");
    if (v && v[0]) {
        return v;
    }
    static char buf[512];
    if (!buf[0]) {
        snprintf(buf, sizeof(buf), "%s/.cache", get_home());
    }
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
    if (path) {
        snprintf(path, len, "%s/isde/%s", base, suffix);
    }
    return path;
}

char *isde_xdg_data_path(const char *suffix)
{
    const char *base = isde_xdg_data_home();
    size_t len = strlen(base) + strlen("/isde/") + strlen(suffix) + 1;
    char *path = malloc(len);
    if (path) {
        snprintf(path, len, "%s/isde/%s", base, suffix);
    }
    return path;
}

/* Search a colon-separated list of directories for isde/<name> */
static char *find_in_dirs(const char *home, const char *dirs, const char *name)
{
    /* Check home first */
    size_t hlen = strlen(home) + strlen("/isde/") + strlen(name) + 1;
    char *path = malloc(hlen);
    if (!path) {
        return NULL;
    }
    snprintf(path, hlen, "%s/isde/%s", home, name);
    if (access(path, R_OK) == 0) {
        return path;
    }
    free(path);

    /* Walk colon-separated dirs */
    const char *p = dirs;
    while (p && *p) {
        const char *colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        if (dlen > 0) {
            size_t total = dlen + strlen("/isde/") + strlen(name) + 1;
            path = malloc(total);
            if (!path) {
                return NULL;
            }
            snprintf(path, total, "%.*s/isde/%s", (int)dlen, p, name);
            if (access(path, R_OK) == 0) {
                return path;
            }
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
    /* Absolute path (e.g. Icon=/usr/share/pixmaps/foo.png): use directly */
    if (name[0] == '/') {
        if (access(name, R_OK) == 0) {
            return strdup(name);
        }
        return NULL;
    }

    /* Read configured icon theme (default: isde-standard) */
    char theme_buf[128] = "isde-standard";
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

    /* If a non-default theme was active, fall back to isde-standard so ISDE's
     * own UI icons are always available regardless of the user's theme choice. */
    if (strcmp(theme_buf, "isde-standard") != 0) {
        path = isde_icon_theme_lookup("isde-standard", category, name);
        if (path) {
            return path;
        }
    }

    /* Freedesktop fallback: /usr/share/pixmaps (unthemed legacy icons).
     * Only check formats ISW can render (SVG and PNG). */
    {
        const char *exts[] = { ".svg", ".png", NULL };
        char pix_path[512];
        for (int i = 0; exts[i]; i++) {
            snprintf(pix_path, sizeof(pix_path), "/usr/share/pixmaps/%s%s",
                     name, exts[i]);
            if (access(pix_path, R_OK) == 0) {
                return strdup(pix_path);
            }
        }
    }

    /* Dev build fallback: isde-standard not yet installed; check source tree. */
    static char exe_dir[512] = {0};
    if (!exe_dir[0]) {
        ssize_t len = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
        if (len > 0) {
            exe_dir[len] = '\0';
            char *slash = strrchr(exe_dir, '/');
            if (slash) { *slash = '\0'; }
        }
    }
    if (exe_dir[0]) {
        char dev_path[512];
        snprintf(dev_path, sizeof(dev_path),
                 "%s/../../common/data/icons/isde-standard/%s/%s.svg",
                 exe_dir, category, name);
        if (access(dev_path, R_OK) == 0) {
            return strdup(dev_path);
        }
    }

    return NULL;
}

char *isde_xdg_user_dir(const char *name)
{
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    char *result = NULL;

    /* Try to read from user-dirs.dirs */
    char path[512];
    snprintf(path, sizeof(path), "%s/user-dirs.dirs", isde_xdg_config_home());

    FILE *fp = fopen(path, "r");
    if (fp) {
        char key[64];
        snprintf(key, sizeof(key), "XDG_%s_DIR", name);
        size_t klen = strlen(key);

        char line[1024];
        while (fgets(line, sizeof(line), fp)) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '#' || *p == '\n' || *p == '\0')
                continue;
            if (strncmp(p, key, klen) != 0 || p[klen] != '=')
                continue;

            p += klen + 1;
            while (*p == ' ' || *p == '\t') p++;
            if (*p != '"')
                continue;
            p++;

            char *end = strchr(p, '"');
            if (!end)
                continue;
            *end = '\0';

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
    }

    /* Fall back to XDG default paths when user-dirs.dirs is missing or
     * doesn't contain the requested key. */
    if (!result) {
        static const struct { const char *key; const char *suffix; } defaults[] = {
            { "DESKTOP",     "/Desktop" },
            { "DOCUMENTS",   "/Documents" },
            { "DOWNLOAD",    "/Downloads" },
            { "MUSIC",       "/Music" },
            { "PICTURES",    "/Pictures" },
            { "VIDEOS",      "/Videos" },
            { "PUBLICSHARE", "/Public" },
            { "TEMPLATES",   "/Templates" },
        };
        for (size_t i = 0; i < sizeof(defaults)/sizeof(defaults[0]); i++) {
            if (strcmp(name, defaults[i].key) == 0) {
                size_t total = strlen(home) + strlen(defaults[i].suffix) + 1;
                result = malloc(total);
                if (result)
                    snprintf(result, total, "%s%s", home, defaults[i].suffix);
                break;
            }
        }
    }

    return result;
}


