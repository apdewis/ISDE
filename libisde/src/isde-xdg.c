/*
 * isde-xdg.c — XDG Base Directory Specification helpers
 */
#include "isde/isde-xdg.h"

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
    char rel[256];
    snprintf(rel, sizeof(rel), "icons/%s/%s.svg", category, name);

    /* Search XDG data dirs for isde/icons/<category>/<name>.svg */
    char *path = isde_xdg_find_data(rel);
    if (path)
        return path;

    /* Fallback: check relative to executable for development builds */
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
        /* Try ../../common/data/icons/ relative to build/fm/ */
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
