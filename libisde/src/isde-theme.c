#define _POSIX_C_SOURCE 200809L
/*
 * isde-theme.c — theme scanning: colour schemes, cursor themes, icon themes
 */
#include "isde/isde-theme.h"
#include "isde/isde-config.h"
#include "isde/isde-xdg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <ctype.h>

/* ---------- helpers ---------- */

static unsigned int parse_hex_color(const char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '#') s++;
    if (*s == '"') s++;
    unsigned int val = 0;
    for (int i = 0; i < 6 && *s; i++, s++) {
        val <<= 4;
        if (*s >= '0' && *s <= '9') val |= *s - '0';
        else if (*s >= 'a' && *s <= 'f') val |= *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') val |= *s - 'A' + 10;
    }
    return val;
}

static void add_name(char ***names, int *count, int *cap, const char *name)
{
    /* Check for duplicates */
    for (int i = 0; i < *count; i++)
        if (strcmp((*names)[i], name) == 0) return;

    if (*count >= *cap) {
        *cap *= 2;
        *names = realloc(*names, *cap * sizeof(char *));
    }
    (*names)[(*count)++] = strdup(name);
}

/* ---------- colour scheme ---------- */

/* Resolve a theme file path, checking XDG data dirs then dev build fallback */
static char *find_theme_file(const char *rel)
{
    char *path = isde_xdg_find_data(rel);
    if (path) return path;

    /* Dev build fallback: check relative to executable */
    char exe_dir[512] = {0};
    ssize_t len = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
    if (len > 0) {
        exe_dir[len] = '\0';
        char *slash = strrchr(exe_dir, '/');
        if (slash) *slash = '\0';
        char devpath[512];
        snprintf(devpath, sizeof(devpath),
                 "%s/../../common/data/%s", exe_dir, rel);
        if (access(devpath, R_OK) == 0)
            return strdup(devpath);
    }
    return NULL;
}

IsdeColorScheme *isde_scheme_load(const char *name)
{
    /* Build filename: <name>.theme */
    char rel[256];
    snprintf(rel, sizeof(rel), "themes/%s.theme", name);

    char *path = find_theme_file(rel);
    if (!path) return NULL;

    FILE *fp = fopen(path, "r");
    free(path);
    if (!fp) return NULL;

    IsdeColorScheme *scheme = calloc(1, sizeof(*scheme));
    char line[256];
    int in_colors = 0, in_scheme = 0;

    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing whitespace */
        char *end = line + strlen(line);
        while (end > line && isspace((unsigned char)end[-1])) *--end = '\0';

        /* Section headers */
        if (line[0] == '[') {
            in_colors = (strcmp(line, "[Colors]") == 0);
            in_scheme = (strcmp(line, "[Color Scheme]") == 0);
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;

        if (in_scheme) {
            if (strcmp(key, "Name") == 0)
                scheme->name = strdup(val);
            else if (strcmp(key, "Author") == 0)
                scheme->author = strdup(val);
        } else if (in_colors) {
            static const struct { const char *name; IsdeColorRole role; } color_keys[] = {
                { "Background",          ISDE_COLOR_BG },
                { "BackgroundLight",     ISDE_COLOR_BG_LIGHT },
                { "SelectionBackground", ISDE_COLOR_SELECT_BG },
                { "Border",              ISDE_COLOR_COMMENT },
                { "ForegroundDim",       ISDE_COLOR_FG_DIM },
                { "Foreground",          ISDE_COLOR_FG },
                { "ForegroundLight",     ISDE_COLOR_FG_LIGHT },
                { "BackgroundBright",    ISDE_COLOR_BG_BRIGHT },
                { "Red",                 ISDE_COLOR_RED },
                { "Orange",              ISDE_COLOR_ORANGE },
                { "Yellow",              ISDE_COLOR_YELLOW },
                { "Green",               ISDE_COLOR_GREEN },
                { "Cyan",                ISDE_COLOR_CYAN },
                { "Blue",                ISDE_COLOR_BLUE },
                { "Purple",              ISDE_COLOR_PURPLE },
                { "Brown",              ISDE_COLOR_BROWN },
            };
            for (int k = 0; k < (int)(sizeof(color_keys)/sizeof(color_keys[0])); k++) {
                if (strcmp(key, color_keys[k].name) == 0) {
                    scheme->colors[color_keys[k].role] = parse_hex_color(val);
                    break;
                }
            }
        }
    }

    fclose(fp);

    if (!scheme->name)
        scheme->name = strdup(name);
    return scheme;
}

void isde_scheme_free(IsdeColorScheme *scheme)
{
    if (!scheme) return;
    free(scheme->name);
    free(scheme->author);
    free(scheme);
}

unsigned int isde_scheme_color(const IsdeColorScheme *scheme,
                                IsdeColorRole role)
{
    if (!scheme || role < 0 || role >= ISDE_COLOR_COUNT)
        return 0;
    return scheme->colors[role];
}

static void scan_schemes_in_dir(const char *dir, char ***names,
                                int *count, int *cap)
{
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d))) {
        size_t nlen = strlen(de->d_name);
        if (nlen < 7 || strcmp(de->d_name + nlen - 6, ".theme") != 0)
            continue;
        /* Strip .theme extension for the name */
        char name[256];
        snprintf(name, sizeof(name), "%.*s", (int)(nlen - 6), de->d_name);
        add_name(names, count, cap, name);
    }
    closedir(d);
}

int isde_scheme_list(char ***names)
{
    int count = 0, cap = 16;
    *names = malloc(cap * sizeof(char *));

    /* Scan XDG data dirs */
    char path[512];
    snprintf(path, sizeof(path), "%s/isde/themes", isde_xdg_data_home());
    scan_schemes_in_dir(path, names, &count, &cap);

    const char *dirs = isde_xdg_data_dirs();
    const char *p = dirs;
    while (p && *p) {
        const char *colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        if (dlen > 0) {
            snprintf(path, sizeof(path), "%.*s/isde/themes", (int)dlen, p);
            scan_schemes_in_dir(path, names, &count, &cap);
        }
        p = colon ? colon + 1 : NULL;
    }

    /* Also check relative to executable for dev builds */
    char exe_dir[512] = {0};
    ssize_t len = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
    if (len > 0) {
        exe_dir[len] = '\0';
        char *slash = strrchr(exe_dir, '/');
        if (slash) *slash = '\0';
        snprintf(path, sizeof(path), "%s/../../common/data/themes", exe_dir);
        scan_schemes_in_dir(path, names, &count, &cap);
    }

    return count;
}

/* ---------- cursor themes ---------- */

static void scan_cursor_themes(const char *icons_dir, char ***names,
                               int *count, int *cap)
{
    DIR *d = opendir(icons_dir);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;

        char cursors_path[512];
        snprintf(cursors_path, sizeof(cursors_path), "%s/%s/cursors",
                 icons_dir, de->d_name);
        if (access(cursors_path, R_OK | X_OK) == 0)
            add_name(names, count, cap, de->d_name);
    }
    closedir(d);
}

int isde_cursor_theme_list(char ***names)
{
    int count = 0, cap = 16;
    *names = malloc(cap * sizeof(char *));

    char path[512];

    /* User icons */
    snprintf(path, sizeof(path), "%s/icons", isde_xdg_data_home());
    scan_cursor_themes(path, names, &count, &cap);

    /* Also ~/.icons (traditional location) */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(path, sizeof(path), "%s/.icons", home);
        scan_cursor_themes(path, names, &count, &cap);
    }

    /* System icons */
    const char *dirs = isde_xdg_data_dirs();
    const char *p = dirs;
    while (p && *p) {
        const char *colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        if (dlen > 0) {
            snprintf(path, sizeof(path), "%.*s/icons", (int)dlen, p);
            scan_cursor_themes(path, names, &count, &cap);
        }
        p = colon ? colon + 1 : NULL;
    }

    return count;
}

/* ---------- icon themes ---------- */

static char *read_theme_name(const char *index_path)
{
    FILE *fp = fopen(index_path, "r");
    if (!fp) return NULL;

    char line[512];
    int in_icon_theme = 0;

    while (fgets(line, sizeof(line), fp)) {
        char *end = line + strlen(line);
        while (end > line && isspace((unsigned char)end[-1])) *--end = '\0';

        if (strcmp(line, "[Icon Theme]") == 0) {
            in_icon_theme = 1;
            continue;
        }
        if (line[0] == '[') {
            in_icon_theme = 0;
            continue;
        }
        if (in_icon_theme && strncmp(line, "Name=", 5) == 0) {
            fclose(fp);
            return strdup(line + 5);
        }
    }
    fclose(fp);
    return NULL;
}

static void scan_icon_themes(const char *icons_dir, char ***names,
                             int *count, int *cap)
{
    DIR *d = opendir(icons_dir);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;

        char index_path[512];
        snprintf(index_path, sizeof(index_path), "%s/%s/index.theme",
                 icons_dir, de->d_name);
        char *name = read_theme_name(index_path);
        if (name) {
            add_name(names, count, cap, name);
            free(name);
        }
    }
    closedir(d);
}

int isde_icon_theme_list(char ***names)
{
    int count = 0, cap = 16;
    *names = malloc(cap * sizeof(char *));

    char path[512];

    snprintf(path, sizeof(path), "%s/icons", isde_xdg_data_home());
    scan_icon_themes(path, names, &count, &cap);

    const char *dirs = isde_xdg_data_dirs();
    const char *p = dirs;
    while (p && *p) {
        const char *colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        if (dlen > 0) {
            snprintf(path, sizeof(path), "%.*s/icons", (int)dlen, p);
            scan_icon_themes(path, names, &count, &cap);
        }
        p = colon ? colon + 1 : NULL;
    }

    return count;
}

char *isde_icon_theme_lookup(const char *theme, const char *category,
                              const char *icon_name)
{
    char path[512];

    /* Try scalable first */
    const char *dirs = isde_xdg_data_dirs();
    const char *p = dirs;
    while (p && *p) {
        const char *colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        if (dlen > 0) {
            snprintf(path, sizeof(path),
                     "%.*s/icons/%s/scalable/%s/%s.svg",
                     (int)dlen, p, theme, category, icon_name);
            if (access(path, R_OK) == 0)
                return strdup(path);
        }
        p = colon ? colon + 1 : NULL;
    }

    /* Fallback to hicolor */
    if (strcmp(theme, "hicolor") != 0)
        return isde_icon_theme_lookup("hicolor", category, icon_name);

    return NULL;
}

/* ---------- global active theme ---------- */

static IsdeColorScheme *g_scheme = NULL;

const IsdeColorScheme *isde_theme_current(void)
{
    if (g_scheme) return g_scheme;

    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (!cfg) return NULL;

    IsdeConfigTable *root = isde_config_root(cfg);
    IsdeConfigTable *appear = isde_config_table(root, "appearance");
    if (appear) {
        const char *name = isde_config_string(appear, "color_scheme", NULL);
        if (name)
            g_scheme = isde_scheme_load(name);
    }
    isde_config_free(cfg);

    /* Default to light if no scheme configured */
    if (!g_scheme)
        g_scheme = isde_scheme_load("default-light");

    return g_scheme;
}

void isde_theme_reload(void)
{
    isde_scheme_free(g_scheme);
    g_scheme = NULL;
    isde_theme_current(); /* re-load */
}

void isde_color_to_rgb(unsigned int color, double *r, double *g, double *b)
{
    *r = ((color >> 16) & 0xFF) / 255.0;
    *g = ((color >> 8)  & 0xFF) / 255.0;
    *b = ( color        & 0xFF) / 255.0;
}

/* ---------- Xt fallback resource generation ---------- */

static char *fmt_color(const char *resource, unsigned int rgb)
{
    char *buf = malloc(strlen(resource) + 16);
    sprintf(buf, "%s: #%06X", resource, rgb);
    return buf;
}

char **isde_theme_build_resources(void)
{
    const IsdeColorScheme *s = isde_theme_current();
    if (!s) return NULL;

    unsigned int bg       = s->colors[ISDE_COLOR_BG];
    unsigned int bg_light = s->colors[ISDE_COLOR_BG_LIGHT];
    unsigned int fg       = s->colors[ISDE_COLOR_FG];
    unsigned int fg_dim   = s->colors[ISDE_COLOR_FG_DIM];
    unsigned int border   = s->colors[ISDE_COLOR_COMMENT];
    unsigned int bg_bright= s->colors[ISDE_COLOR_BG_BRIGHT];

    /* Build resource lines — wildcard resources cascade to all widgets */
    char **res = calloc(32, sizeof(char *));
    int i = 0;

    /* Global widget defaults */
    res[i++] = fmt_color("*background", bg);
    res[i++] = fmt_color("*foreground", fg);
    res[i++] = fmt_color("*borderColor", border);

    /* Container and input widgets */
    res[i++] = fmt_color("*Form.background", bg);
    res[i++] = fmt_color("*Box.background", bg);
    res[i++] = fmt_color("*Paned.background", bg);
    res[i++] = fmt_color("*Viewport.background", bg);
    res[i++] = fmt_color("*MainWindow.background", bg);

    /* Buttons */
    res[i++] = fmt_color("*Command.background", bg_light);
    res[i++] = fmt_color("*Command.foreground", fg);

    /* Labels */
    res[i++] = fmt_color("*Label.background", bg);
    res[i++] = fmt_color("*Label.foreground", fg);

    /* Lists and selections */
    res[i++] = fmt_color("*List.background", bg);
    res[i++] = fmt_color("*List.foreground", fg);

    /* Menus */
    res[i++] = fmt_color("*SimpleMenu.background", bg_light);
    res[i++] = fmt_color("*SimpleMenu.foreground", fg);
    res[i++] = fmt_color("*SmeBSB.foreground", fg);

    /* Menu bar buttons */
    res[i++] = fmt_color("*MenuButton.background", bg);
    res[i++] = fmt_color("*MenuButton.foreground", fg);

    /* Scrollbars */
    res[i++] = fmt_color("*Scrollbar.background", bg_light);
    res[i++] = fmt_color("*Scrollbar.foreground", fg_dim);

    /* Dialog */
    res[i++] = fmt_color("*Dialog.background", bg);
    res[i++] = fmt_color("*Dialog.foreground", fg);

    /* Text input */
    res[i++] = fmt_color("*Text.background", bg_bright);
    res[i++] = fmt_color("*Text.foreground", fg);

    /* IconView */
    res[i++] = fmt_color("*IconView.background", bg);
    res[i++] = fmt_color("*IconView.foreground", fg);

    /* StatusBar */
    res[i++] = fmt_color("*StatusBar.background", bg_light);
    res[i++] = fmt_color("*StatusBar.foreground", fg);

    res[i] = NULL;
    return res;
}

void isde_theme_free_resources(char **resources)
{
    if (!resources) return;
    for (int i = 0; resources[i]; i++)
        free(resources[i]);
    free(resources);
}
