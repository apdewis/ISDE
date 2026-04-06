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
    while (*s && isspace((unsigned char)*s)) { s++; }
    if (*s == '#') { s++; }
    if (*s == '"') { s++; }
    unsigned int val = 0;
    for (int i = 0; i < 6 && *s; i++, s++) {
        val <<= 4;
        if (*s >= '0' && *s <= '9') { val |= *s - '0'; }
        else if (*s >= 'a' && *s <= 'f') { val |= *s - 'a' + 10; }
        else if (*s >= 'A' && *s <= 'F') { val |= *s - 'A' + 10; }
    }
    return val;
}

static void add_name(char ***names, int *count, int *cap, const char *name)
{
    /* Check for duplicates */
    for (int i = 0; i < *count; i++) {
        if (strcmp((*names)[i], name) == 0) { return; }
    }

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
    if (path) { return path; }

    /* Dev build fallback: check relative to executable */
    char exe_dir[512] = {0};
    ssize_t len = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
    if (len > 0) {
        exe_dir[len] = '\0';
        char *slash = strrchr(exe_dir, '/');
        if (slash) { *slash = '\0'; }
        char devpath[512];
        snprintf(devpath, sizeof(devpath),
                 "%s/../../common/data/%s", exe_dir, rel);
        if (access(devpath, R_OK) == 0) {
            return strdup(devpath);
        }
    }
    return NULL;
}

/* Parse element colors from key=value within an element section */
static void parse_element(IsdeElementColors *el, const char *key,
                          const char *val)
{
    unsigned int c = parse_hex_color(val);
    if      (strcmp(key, "Background")      == 0) { el->bg       = c; }
    else if (strcmp(key, "Foreground")       == 0) { el->fg       = c; }
    else if (strcmp(key, "Border")           == 0) { el->border   = c; }
    else if (strcmp(key, "HoverBackground")  == 0) { el->hover_bg = c; }
    else if (strcmp(key, "HoverForeground")  == 0) { el->hover_fg = c; }
}

/* Fill in element defaults from global scheme values where not explicitly set */
static void element_defaults(IsdeElementColors *el, unsigned int bg,
                             unsigned int fg, unsigned int border_c)
{
    if (!el->bg)       { el->bg       = bg; }
    if (!el->fg)       { el->fg       = fg; }
    if (!el->border)   { el->border   = border_c; }
    if (!el->hover_bg) { el->hover_bg = el->bg; }
    if (!el->hover_fg) { el->hover_fg = el->fg; }
}

IsdeColorScheme *isde_scheme_load(const char *name)
{
    /* Build filename: <name>.theme */
    char rel[256];
    snprintf(rel, sizeof(rel), "themes/%s.theme", name);

    char *path = find_theme_file(rel);
    if (!path) { return NULL; }

    FILE *fp = fopen(path, "r");
    free(path);
    if (!fp) { return NULL; }

    IsdeColorScheme *s = calloc(1, sizeof(*s));
    char line[256];

    enum {
        SEC_NONE, SEC_SCHEME, SEC_COLORS,
        SEC_TITLEBAR, SEC_TITLEBAR_ACTIVE, SEC_TITLEBAR_BUTTON,
        SEC_CLOSE_BUTTON, SEC_MENU, SEC_MENU_ITEM,
        SEC_TASKBAR, SEC_TASKBAR_BUTTON,
        SEC_TASKBAR_BUTTON_ACTIVE, SEC_TASKBAR_BUTTON_FOCUS
    } section = SEC_NONE;

    while (fgets(line, sizeof(line), fp)) {
        char *end = line + strlen(line);
        while (end > line && isspace((unsigned char)end[-1])) { *--end = '\0'; }

        if (line[0] == '[') {
            if      (strcmp(line, "[Color Scheme]")   == 0) { section = SEC_SCHEME; }
            else if (strcmp(line, "[Colors]")          == 0) { section = SEC_COLORS; }
            else if (strcmp(line, "[TitleBar]")        == 0) { section = SEC_TITLEBAR; }
            else if (strcmp(line, "[TitleBarActive]")  == 0) { section = SEC_TITLEBAR_ACTIVE; }
            else if (strcmp(line, "[TitleBarButton]")  == 0) { section = SEC_TITLEBAR_BUTTON; }
            else if (strcmp(line, "[CloseButton]")     == 0) { section = SEC_CLOSE_BUTTON; }
            else if (strcmp(line, "[Menu]")            == 0) { section = SEC_MENU; }
            else if (strcmp(line, "[MenuItem]")        == 0) { section = SEC_MENU_ITEM; }
            else if (strcmp(line, "[Taskbar]")         == 0) { section = SEC_TASKBAR; }
            else if (strcmp(line, "[TaskbarButton]")       == 0) { section = SEC_TASKBAR_BUTTON; }
            else if (strcmp(line, "[TaskbarButtonActive]") == 0) { section = SEC_TASKBAR_BUTTON_ACTIVE; }
            else if (strcmp(line, "[TaskbarButtonFocus]")  == 0) { section = SEC_TASKBAR_BUTTON_FOCUS; }
            else { section = SEC_NONE; }
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) { continue; }
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;

        switch (section) {
        case SEC_SCHEME:
            if      (strcmp(key, "Name")   == 0) { s->name   = strdup(val); }
            else if (strcmp(key, "Author") == 0) { s->author = strdup(val); }
            break;

        case SEC_COLORS: {
            unsigned int c = parse_hex_color(val);
            if      (strcmp(key, "Background")          == 0) { s->bg        = c; }
            else if (strcmp(key, "BackgroundLight")      == 0) { s->bg_light  = c; }
            else if (strcmp(key, "BackgroundBright")     == 0) { s->bg_bright = c; }
            else if (strcmp(key, "Foreground")           == 0) { s->fg        = c; }
            else if (strcmp(key, "ForegroundDim")        == 0) { s->fg_dim    = c; }
            else if (strcmp(key, "ForegroundLight")      == 0) { s->fg_light  = c; }
            else if (strcmp(key, "Border")               == 0) { s->border    = c; }
            else if (strcmp(key, "SelectionBackground")  == 0) { s->select_bg = c; }
            else if (strcmp(key, "SelectionForeground")  == 0) { s->select_fg = c; }
            else if (strcmp(key, "Error")                == 0) { s->error     = c; }
            else if (strcmp(key, "Warning")              == 0) { s->warning   = c; }
            else if (strcmp(key, "Success")              == 0) { s->success   = c; }
            else if (strcmp(key, "Active")               == 0) { s->active    = c; }
            else if (strcmp(key, "Accent")               == 0) { s->accent    = c; }
            break;
        }

        case SEC_TITLEBAR:        parse_element(&s->titlebar,        key, val); break;
        case SEC_TITLEBAR_ACTIVE: parse_element(&s->titlebar_active, key, val); break;
        case SEC_TITLEBAR_BUTTON: parse_element(&s->titlebar_button, key, val); break;
        case SEC_CLOSE_BUTTON:    parse_element(&s->close_button,    key, val); break;
        case SEC_MENU:            parse_element(&s->menu,            key, val); break;
        case SEC_MENU_ITEM:       parse_element(&s->menu_item,       key, val); break;
        case SEC_TASKBAR:         parse_element(&s->taskbar,         key, val); break;
        case SEC_TASKBAR_BUTTON:        parse_element(&s->taskbar_button,        key, val); break;
        case SEC_TASKBAR_BUTTON_ACTIVE: parse_element(&s->taskbar_button_active, key, val); break;
        case SEC_TASKBAR_BUTTON_FOCUS:  parse_element(&s->taskbar_button_focus,  key, val); break;
        default: break;
        }
    }

    fclose(fp);

    if (!s->name) {
        s->name = strdup(name);
    }

    /* Fill defaults for element sections that weren't specified */
    element_defaults(&s->titlebar,        s->bg_light, s->fg,       s->border);
    element_defaults(&s->titlebar_active, s->active,   s->fg_light, s->active);
    element_defaults(&s->titlebar_button, s->bg_light, s->fg,       s->border);
    element_defaults(&s->close_button,    s->error,    s->fg_light, s->error);
    element_defaults(&s->menu,            s->bg,       s->fg,       s->border);
    element_defaults(&s->menu_item,       s->bg,       s->fg,       s->bg);
    element_defaults(&s->taskbar,              s->bg_light,  s->fg,       s->border);
    element_defaults(&s->taskbar_button,       s->bg_light,  s->fg,       s->border);
    element_defaults(&s->taskbar_button_active,s->select_bg, s->fg,       s->border);
    element_defaults(&s->taskbar_button_focus, s->active,    s->fg_light, s->active);

    return s;
}

void isde_scheme_free(IsdeColorScheme *scheme)
{
    if (!scheme) { return; }
    free(scheme->name);
    free(scheme->author);
    free(scheme);
}

static void scan_schemes_in_dir(const char *dir, char ***names,
                                int *count, int *cap)
{
    DIR *d = opendir(dir);
    if (!d) { return; }

    struct dirent *de;
    while ((de = readdir(d))) {
        size_t nlen = strlen(de->d_name);
        if (nlen < 7 || strcmp(de->d_name + nlen - 6, ".theme") != 0) {
            continue;
        }
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
        if (slash) { *slash = '\0'; }
        snprintf(path, sizeof(path), "%s/../../common/data/themes", exe_dir);
        scan_schemes_in_dir(path, names, &count, &cap);
    }

    return count;
}

/* ---------- cursor themes ---------- */

/* Check whether an index.theme has a Directories= line (icon theme) */
static int theme_has_directories(const char *icons_dir, const char *theme_name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/index.theme", icons_dir, theme_name);
    FILE *fp = fopen(path, "r");
    if (!fp) { return 0; }
    char line[4096];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Directories=", 12) == 0) { found = 1; break; }
    }
    fclose(fp);
    return found;
}

static void scan_cursor_themes(const char *icons_dir, char ***names,
                               int *count, int *cap)
{
    DIR *d = opendir(icons_dir);
    if (!d) { return; }

    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') { continue; }

        /* Must have a cursors/ subdirectory */
        char cursors_path[512];
        snprintf(cursors_path, sizeof(cursors_path), "%s/%s/cursors",
                 icons_dir, de->d_name);
        if (access(cursors_path, R_OK | X_OK) != 0) {
            continue;
        }

        /* Exclude icon themes (have Directories= in index.theme) */
        if (theme_has_directories(icons_dir, de->d_name)) {
            continue;
        }

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
    if (!fp) { return NULL; }

    char line[512];
    int in_icon_theme = 0;

    while (fgets(line, sizeof(line), fp)) {
        char *end = line + strlen(line);
        while (end > line && isspace((unsigned char)end[-1])) { *--end = '\0'; }

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
    if (!d) { return; }

    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') { continue; }

        /* Must have Directories= to be an icon theme */
        if (!theme_has_directories(icons_dir, de->d_name)) {
            continue;
        }

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

/* Find the base directory containing a theme (where index.theme lives) */
static char *find_theme_base(const char *theme)
{
    char path[512];

    /* User icons */
    snprintf(path, sizeof(path), "%s/icons/%s/index.theme",
             isde_xdg_data_home(), theme);
    if (access(path, R_OK) == 0) {
        snprintf(path, sizeof(path), "%s/icons/%s",
                 isde_xdg_data_home(), theme);
        return strdup(path);
    }

    /* ~/.icons (traditional) */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(path, sizeof(path), "%s/.icons/%s/index.theme", home, theme);
        if (access(path, R_OK) == 0) {
            snprintf(path, sizeof(path), "%s/.icons/%s", home, theme);
            return strdup(path);
        }
    }

    /* System dirs */
    const char *dirs = isde_xdg_data_dirs();
    const char *p = dirs;
    while (p && *p) {
        const char *colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        if (dlen > 0) {
            snprintf(path, sizeof(path), "%.*s/icons/%s/index.theme",
                     (int)dlen, p, theme);
            if (access(path, R_OK) == 0) {
                snprintf(path, sizeof(path), "%.*s/icons/%s",
                         (int)dlen, p, theme);
                return strdup(path);
            }
        }
        p = colon ? colon + 1 : NULL;
    }

    return NULL;
}

/* Try to find an icon file in a single base directory, checking
 * every subdirectory listed in index.theme's Directories= line.
 * Tries: exact.svg, exact.png, exact-symbolic.svg for each dir. */
static char *search_theme_base(const char *base, const char *icon_name)
{
    char idx_path[512];
    snprintf(idx_path, sizeof(idx_path), "%s/index.theme", base);

    FILE *fp = fopen(idx_path, "r");
    if (!fp) { return NULL; }

    char *directories = NULL;
    char line[8192];
    int in_theme = 0;

    while (fgets(line, sizeof(line), fp)) {
        char *end = line + strlen(line);
        while (end > line && isspace((unsigned char)end[-1])) { *--end = '\0'; }

        if (strcmp(line, "[Icon Theme]") == 0) { in_theme = 1; continue; }
        if (line[0] == '[') { in_theme = 0; continue; }
        if (in_theme && strncmp(line, "Directories=", 12) == 0) {
            directories = strdup(line + 12);
            break;
        }

    }
    fclose(fp);

    if (!directories) { return NULL; }

    /* Names to try: exact name, then -symbolic variant */
    const char *names[2];
    names[0] = icon_name;
    char symbolic[256];
    snprintf(symbolic, sizeof(symbolic), "%s-symbolic", icon_name);
    names[1] = symbolic;

    char path[512];

    for (int ni = 0; ni < 2; ni++) {
        /* Pass 1: prefer SVG (resolution-independent) */
        char *dp = directories;
        while (dp && *dp) {
            char *comma = strchr(dp, ',');
            size_t dlen = comma ? (size_t)(comma - dp) : strlen(dp);
            if (dlen > 0) {
                snprintf(path, sizeof(path), "%s/%.*s/%s.svg",
                         base, (int)dlen, dp, names[ni]);
                if (access(path, R_OK) == 0) {
                    free(directories);
                    return strdup(path);
                }
            }
            dp = comma ? comma + 1 : NULL;
        }

        /* Pass 2: pick the largest available raster icon (scaling down
         * from a larger source looks much better than scaling up) */
        char best[512] = {0};
        int best_size = 0;
        dp = directories;
        while (dp && *dp) {
            char *comma = strchr(dp, ',');
            size_t dlen = comma ? (size_t)(comma - dp) : strlen(dp);
            if (dlen > 0) {
                snprintf(path, sizeof(path), "%s/%.*s/%s.png",
                         base, (int)dlen, dp, names[ni]);
                if (access(path, R_OK) == 0) {
                    /* Parse size from directory name (e.g. "48x48/apps") */
                    int size = atoi(dp);
                    if (size > best_size) {
                        best_size = size;
                        snprintf(best, sizeof(best), "%s", path);
                    } else if (best[0] == '\0') {
                        /* Non-numeric dir name (e.g. "symbolic") — keep as
                         * fallback only if we have nothing yet */
                        snprintf(best, sizeof(best), "%s", path);
                    }
                }
            }
            dp = comma ? comma + 1 : NULL;
        }
        if (best[0]) {
            free(directories);
            return strdup(best);
        }
    }

    free(directories);
    return NULL;
}

/* Read the Inherits= line from a theme's index.theme */
static char *read_theme_inherits(const char *base)
{
    char idx_path[512];
    snprintf(idx_path, sizeof(idx_path), "%s/index.theme", base);

    FILE *fp = fopen(idx_path, "r");
    if (!fp) { return NULL; }

    char *inherits = NULL;
    char line[1024];
    int in_theme = 0;

    while (fgets(line, sizeof(line), fp)) {
        char *end = line + strlen(line);
        while (end > line && isspace((unsigned char)end[-1])) { *--end = '\0'; }

        if (strcmp(line, "[Icon Theme]") == 0) { in_theme = 1; continue; }
        if (line[0] == '[') { in_theme = 0; continue; }
        if (in_theme && strncmp(line, "Inherits=", 9) == 0) {
            inherits = strdup(line + 9);
            break;
        }
    }
    fclose(fp);
    return inherits;
}

char *isde_icon_theme_lookup(const char *theme, const char *category,
                              const char *icon_name)
{
    (void)category;

    /* Prevent infinite loops in broken theme chains */
    static int depth = 0;
    if (depth > 8) { return NULL; }

    char *base = find_theme_base(theme);
    if (!base) {
        /* Theme not found — try hicolor as last resort */
        if (strcmp(theme, "hicolor") != 0) {
            depth++;
            char *r = isde_icon_theme_lookup("hicolor", category, icon_name);
            depth--;
            return r;
        }
        return NULL;
    }

    /* Search this theme's directories */
    char *path = search_theme_base(base, icon_name);
    if (path) {
        free(base);
        return path;
    }

    /* Follow Inherits= chain */
    char *inherits = read_theme_inherits(base);
    free(base);

    if (inherits) {
        char *ip = inherits;
        while (ip && *ip) {
            char *comma = strchr(ip, ',');
            size_t ilen = comma ? (size_t)(comma - ip) : strlen(ip);
            if (ilen > 0) {
                char parent[256];
                snprintf(parent, sizeof(parent), "%.*s", (int)ilen, ip);
                depth++;
                path = isde_icon_theme_lookup(parent, category, icon_name);
                depth--;
                if (path) {
                    free(inherits);
                    return path;
                }
            }
            ip = comma ? comma + 1 : NULL;
        }
        free(inherits);
    }

    /* Final fallback to hicolor if not already tried */
    if (strcmp(theme, "hicolor") != 0) {
        depth++;
        path = isde_icon_theme_lookup("hicolor", category, icon_name);
        depth--;
        return path;
    }

    return NULL;
}

/* ---------- global active theme ---------- */

static IsdeColorScheme *g_scheme = NULL;
static char *g_cursor_theme = NULL;
static char *g_cursor_size  = NULL;

const IsdeColorScheme *isde_theme_current(void)
{
    if (g_scheme) { return g_scheme; }

    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (!cfg) { return NULL; }

    IsdeConfigTable *root = isde_config_root(cfg);
    IsdeConfigTable *appear = isde_config_table(root, "appearance");
    if (appear) {
        const char *name = isde_config_string(appear, "color_scheme", NULL);
        if (name) {
            g_scheme = isde_scheme_load(name);
        }
        const char *ct = isde_config_string(appear, "cursor_theme", NULL);
        free(g_cursor_theme);
        g_cursor_theme = ct ? strdup(ct) : NULL;
        const char *cs = isde_config_string(appear, "cursor_size", NULL);
        free(g_cursor_size);
        g_cursor_size = cs ? strdup(cs) : NULL;
    }
    isde_config_free(cfg);

    /* Default to light if no scheme configured */
    if (!g_scheme) {
        g_scheme = isde_scheme_load("default-light");
    }

    return g_scheme;
}

void isde_theme_reload(void)
{
    isde_scheme_free(g_scheme);
    g_scheme = NULL;
    isde_theme_current(); /* re-load */
}

const char *isde_cursor_theme_configured(void)
{
    isde_theme_current(); /* ensure config is loaded */
    return g_cursor_theme;
}

const char *isde_cursor_size_configured(void)
{
    isde_theme_current();
    return g_cursor_size;
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

static char *fmt_font(const char *resource, const char *family, int size)
{
    char *buf = malloc(strlen(resource) + strlen(family) + 16);
    sprintf(buf, "%s: %s-%d", resource, family, size);
    return buf;
}

static char *fmt_font_bold(const char *resource, const char *family, int size)
{
    char *buf = malloc(strlen(resource) + strlen(family) + 32);
    sprintf(buf, "%s: %s-%d:bold", resource, family, size);
    return buf;
}

char **isde_theme_build_resources(void)
{
    const IsdeColorScheme *s = isde_theme_current();
    if (!s) { return NULL; }

    char **res = calloc(128, sizeof(char *));
    int i = 0;

    /* Global defaults */
    res[i++] = fmt_color("*background", s->bg);
    res[i++] = fmt_color("*foreground", s->fg);
    res[i++] = fmt_color("*borderColor", s->border);

    /* Window borders — 1px in fg color */
    res[i++] = fmt_color("*TransientShell.borderColor", s->fg);
    res[i++] = fmt_color("*TopLevelShell.borderColor", s->fg);

    /* Containers */
    res[i++] = fmt_color("*Form.background", s->bg);
    res[i++] = fmt_color("*Box.background", s->bg);
    res[i++] = fmt_color("*Paned.background", s->bg);
    res[i++] = fmt_color("*Viewport.background", s->bg);
    res[i++] = fmt_color("*MainWindow.background", s->bg);

    /* Buttons — use X11 border, not highlight_thickness */
    res[i++] = fmt_color("*Command.background", s->bg_light);
    res[i++] = fmt_color("*Command.foreground", s->fg);
    res[i++] = fmt_color("*Command.borderColor", s->border);
    res[i++] = strdup("*Command.borderStrokeWidth: 0");

    /* Labels */
    res[i++] = fmt_color("*Label.background", s->bg);
    res[i++] = fmt_color("*Label.foreground", s->fg);

    /* Lists */
    res[i++] = fmt_color("*List.background", s->bg_bright);
    res[i++] = fmt_color("*List.foreground", s->fg);

    /* Menus */
    res[i++] = fmt_color("*SimpleMenu.background", s->menu.bg);
    res[i++] = fmt_color("*SimpleMenu.foreground", s->menu.fg);
    res[i++] = fmt_color("*SimpleMenu.borderColor", s->menu.border);
    res[i++] = fmt_color("*SmeBSB.foreground", s->menu_item.fg);

    /* MenuBar buttons */
    res[i++] = fmt_color("*MenuButton.background", s->bg);
    res[i++] = fmt_color("*MenuButton.foreground", s->fg);

    /* Scrollbars */
    res[i++] = fmt_color("*Scrollbar.background", s->bg_light);
    res[i++] = fmt_color("*Scrollbar.foreground", s->fg_dim);

    /* Dialog */
    res[i++] = fmt_color("*Dialog.background", s->bg);
    res[i++] = fmt_color("*Dialog.foreground", s->fg);

    /* Text input */
    res[i++] = fmt_color("*Text.background", s->bg_bright);
    res[i++] = fmt_color("*Text.foreground", s->fg);

    /* IconView */
    res[i++] = fmt_color("*IconView.background", s->bg);
    res[i++] = fmt_color("*IconView.foreground", s->fg);

    /* StatusBar */
    res[i++] = fmt_color("*StatusBar.background", s->bg_light);
    res[i++] = fmt_color("*StatusBar.foreground", s->fg);

    /* Panel / Taskbar */
    res[i++] = fmt_color("*panelBox.background", s->taskbar.bg);
    res[i++] = fmt_color("*startBtn.background", s->taskbar_button.bg);
    res[i++] = fmt_color("*startBtn.foreground", s->taskbar_button.fg);
    res[i++] = fmt_color("*startBtn.borderColor", s->taskbar_button.border);
    res[i++] = fmt_color("*taskBtn.background", s->taskbar_button.bg);
    res[i++] = fmt_color("*taskBtn.foreground", s->taskbar_button.fg);
    res[i++] = fmt_color("*taskBtn.borderColor", s->taskbar_button.border);

    /* Start menu */
    res[i++] = fmt_color("*startMenu.background", s->menu.bg);
    res[i++] = fmt_color("*startMenu.borderColor", s->menu.border);
    res[i++] = fmt_color("*catList.background", s->menu.bg);
    res[i++] = fmt_color("*catList.foreground", s->menu.fg);
    res[i++] = fmt_color("*appList.background", s->menu.bg);
    res[i++] = fmt_color("*appList.foreground", s->menu.fg);

    /* Window list popup */
    res[i++] = fmt_color("*winListMenu.background", s->menu.bg);
    res[i++] = fmt_color("*winListMenu.borderColor", s->menu.border);
    res[i++] = fmt_color("*winList.background", s->menu.bg);
    res[i++] = fmt_color("*winList.foreground", s->menu.fg);

    /* Context menus */
    res[i++] = fmt_color("*ctxMenu.background", s->menu.bg);
    res[i++] = fmt_color("*ctxMenu.borderColor", s->menu.border);
    res[i++] = fmt_color("*ctxList.background", s->menu.bg);
    res[i++] = fmt_color("*ctxList.foreground", s->menu.fg);

    /* Clock */
    res[i++] = fmt_color("*clockTime.background", s->taskbar.bg);
    res[i++] = fmt_color("*clockTime.foreground", s->taskbar.fg);
    res[i++] = fmt_color("*clockDate.background", s->taskbar.bg);
    res[i++] = fmt_color("*clockDate.foreground", s->taskbar.fg);

    /* WM title bar (unfocused defaults — focused set via XtSetValues) */
    res[i++] = fmt_color("*titleBar.background", s->titlebar.bg);
    res[i++] = fmt_color("*titleBar.foreground", s->titlebar.fg);
    res[i++] = fmt_color("*frame.minimizeBtn.background", s->titlebar_button.bg);
    res[i++] = fmt_color("*frame.minimizeBtn.foreground", s->titlebar_button.fg);
    res[i++] = fmt_color("*frame.minimizeBtn.borderColor", s->titlebar_button.border);
    res[i++] = fmt_color("*frame.maximizeBtn.background", s->titlebar_button.bg);
    res[i++] = fmt_color("*frame.maximizeBtn.foreground", s->titlebar_button.fg);
    res[i++] = fmt_color("*frame.maximizeBtn.borderColor", s->titlebar_button.border);
    res[i++] = fmt_color("*frame.closeBtn.background", s->close_button.bg);
    res[i++] = fmt_color("*frame.closeBtn.foreground", s->close_button.fg);
    res[i++] = fmt_color("*frame.closeBtn.borderColor", s->close_button.border);

    /* Frame shell background (visible as border around client) */
    res[i++] = fmt_color("*frame.background", s->titlebar.border);

    /* Places sidebar — lighter background */
    res[i++] = fmt_color("*placesVp.background", s->bg_light);
    res[i++] = fmt_color("*placesBox.background", s->bg_light);
    res[i++] = fmt_color("*placeList0.background", s->bg_light);
    res[i++] = fmt_color("*placeList1.background", s->bg_light);
    res[i++] = fmt_color("*placeList2.background", s->bg_light);
    res[i++] = fmt_color("*placeHdr0.background", s->bg_light);
    res[i++] = fmt_color("*placeHdr1.background", s->bg_light);
    res[i++] = fmt_color("*placeHdr2.background", s->bg_light);
    res[i++] = fmt_color("*placeHdr0.foreground", s->fg_dim);
    res[i++] = fmt_color("*placeHdr1.foreground", s->fg_dim);
    res[i++] = fmt_color("*placeHdr2.foreground", s->fg_dim);

    /* Navbar — lighter background, dark border */
    res[i++] = fmt_color("*navBar.background", s->bg_light);
    res[i++] = fmt_color("*navBar.borderColor", s->fg);
    res[i++] = fmt_color("*placesVp.borderColor", s->fg);

    /* Taskbar — square buttons */
    res[i++] = strdup("*taskBtn.cornerRadius: 0");
    res[i++] = strdup("*startBtn.cornerRadius: 0");

    /* --- Fonts from [fonts] config --- */
    {
        char errbuf[256];
        IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf,
                                                sizeof(errbuf));
        if (cfg) {
            IsdeConfigTable *root = isde_config_root(cfg);
            IsdeConfigTable *fonts = isde_config_table(root, "fonts");
            if (fonts) {
                const char *fam;
                int sz;

                /* General — global default */
                fam = isde_config_string(fonts, "general_family", "Sans");
                sz  = (int)isde_config_int(fonts, "general_size", 10);
                res[i++] = fmt_font("*font", fam, sz);

                /* Section headings — general +2pt bold */
                res[i++] = fmt_font_bold("*sectionHd.font", fam, sz + 2);

                /* Section headings — general +2pt bold */
                res[i++] = fmt_font_bold("*sectionHd.font", fam, sz + 2);

                /* Fixed — Text widget (editors, terminal) */
                fam = isde_config_string(fonts, "fixed_family", "Monospace");
                sz  = (int)isde_config_int(fonts, "fixed_size", 10);
                res[i++] = fmt_font("*Text.font", fam, sz);
                res[i++] = fmt_font("*AsciiSink.font", fam, sz);

                /* Places sidebar headers — 2pt smaller than general */
                res[i++] = fmt_font("*placeHdr0.font", fam, sz - 2);
                res[i++] = fmt_font("*placeHdr1.font", fam, sz - 2);
                res[i++] = fmt_font("*placeHdr2.font", fam, sz - 2);

                /* Small — StatusBar, Tip */
                fam = isde_config_string(fonts, "small_family", "Sans");
                sz  = (int)isde_config_int(fonts, "small_size", 8);
                res[i++] = fmt_font("*StatusBar.font", fam, sz);
                res[i++] = fmt_font("*Tip.font", fam, sz);

                /* Toolbar — Command buttons in toolbars */
                fam = isde_config_string(fonts, "toolbar_family", "Sans");
                sz  = (int)isde_config_int(fonts, "toolbar_size", 9);
                res[i++] = fmt_font("*startBtn.font", fam, sz);
                res[i++] = fmt_font("*taskBtn.font", fam, sz);

                /* Menus */
                fam = isde_config_string(fonts, "menu_family", "Sans");
                sz  = (int)isde_config_int(fonts, "menu_size", 10);
                res[i++] = fmt_font("*SmeBSB.font", fam, sz);
                res[i++] = fmt_font("*MenuButton.font", fam, sz);

                /* Window title */
                fam = isde_config_string(fonts, "title_family", "Sans");
                sz  = (int)isde_config_int(fonts, "title_size", 10);
                res[i++] = fmt_font("*titleBar.font", fam, sz);
            }
            isde_config_free(cfg);
        }
    }

    res[i] = NULL;
    return res;
}

void isde_theme_free_resources(char **resources)
{
    if (!resources) { return; }
    for (int i = 0; resources[i]; i++) {
        free(resources[i]);
    }
    free(resources);
}

/* Cached font sizes — loaded once from config */
static int g_font_sizes[6] = {0};  /* general, fixed, small, toolbar, menu, title */
static int g_font_sizes_loaded = 0;

static void load_font_sizes(void)
{
    if (g_font_sizes_loaded) { return; }
    g_font_sizes_loaded = 1;

    /* Defaults */
    g_font_sizes[0] = 10; /* general */
    g_font_sizes[1] = 10; /* fixed */
    g_font_sizes[2] = 8;  /* small */
    g_font_sizes[3] = 9;  /* toolbar */
    g_font_sizes[4] = 10; /* menu */
    g_font_sizes[5] = 10; /* title */

    static const char *keys[6] = {
        "general_size", "fixed_size", "small_size",
        "toolbar_size", "menu_size", "title_size"
    };

    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *fonts = isde_config_table(root, "fonts");
        if (fonts) {
            for (int i = 0; i < 6; i++) {
                int sz = (int)isde_config_int(fonts, keys[i], 0);
                if (sz > 0) { g_font_sizes[i] = sz; }
            }
        }
        isde_config_free(cfg);
    }
}

int isde_font_height(const char *category, int padding)
{
    load_font_sizes();

    int idx = 0; /* general */
    if      (strcmp(category, "fixed")   == 0) { idx = 1; }
    else if (strcmp(category, "small")   == 0) { idx = 2; }
    else if (strcmp(category, "toolbar") == 0) { idx = 3; }
    else if (strcmp(category, "menu")    == 0) { idx = 4; }
    else if (strcmp(category, "title")   == 0) { idx = 5; }

    /* Convert point size to pixel height:
     * pixels = pt * (96 / 72) = pt * 4/3, then add padding and scale */
    int px = (g_font_sizes[idx] * 4 + 2) / 3 + padding;
    return isde_scale(px);
}
