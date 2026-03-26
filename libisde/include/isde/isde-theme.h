/*
 * isde-theme.h — theme scanning and colour scheme loading
 *
 * Colour schemes: .theme INI files in $XDG_DATA_DIRS/isde/themes/
 * Cursor themes: freedesktop Xcursor in $XDG_DATA_DIRS/icons/{theme}/cursors/
 * Icon themes: freedesktop Icon Theme Spec in $XDG_DATA_DIRS/icons/{theme}/index.theme
 */
#ifndef ISDE_THEME_H
#define ISDE_THEME_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Colour scheme (base16) ---------- */

/* Base16 colour roles */
typedef enum {
    ISDE_COLOR_BG = 0,         /* base00 — default background */
    ISDE_COLOR_BG_LIGHT,       /* base01 — lighter background */
    ISDE_COLOR_SELECT_BG,      /* base02 — selection background */
    ISDE_COLOR_COMMENT,        /* base03 — comments, disabled, borders */
    ISDE_COLOR_FG_DIM,         /* base04 — dark foreground */
    ISDE_COLOR_FG,             /* base05 — default foreground */
    ISDE_COLOR_FG_LIGHT,       /* base06 — light foreground */
    ISDE_COLOR_BG_BRIGHT,      /* base07 — bright background */
    ISDE_COLOR_RED,            /* base08 — errors, close button */
    ISDE_COLOR_ORANGE,         /* base09 */
    ISDE_COLOR_YELLOW,         /* base0A */
    ISDE_COLOR_GREEN,          /* base0B */
    ISDE_COLOR_CYAN,           /* base0C */
    ISDE_COLOR_BLUE,           /* base0D — active, links, focus */
    ISDE_COLOR_PURPLE,         /* base0E */
    ISDE_COLOR_BROWN,          /* base0F */
    ISDE_COLOR_COUNT
} IsdeColorRole;

typedef struct {
    char    *name;
    char    *author;
    unsigned int colors[ISDE_COLOR_COUNT]; /* 0xRRGGBB */
} IsdeColorScheme;

/* Load a named colour scheme from the theme directories.
 * Returns NULL if not found. Caller must free with isde_scheme_free(). */
IsdeColorScheme *isde_scheme_load(const char *name);
void             isde_scheme_free(IsdeColorScheme *scheme);

/* Get a color value (0xRRGGBB) from the current/loaded scheme. */
unsigned int     isde_scheme_color(const IsdeColorScheme *scheme,
                                    IsdeColorRole role);

/* List available colour scheme names.
 * Returns count; *names is a malloc'd array of malloc'd strings.
 * Caller must free each string and the array. */
int isde_scheme_list(char ***names);

/* ---------- Cursor themes ---------- */

/* List available Xcursor theme names.
 * Scans $XDG_DATA_DIRS/icons/{theme}/cursors/ directories. */
int isde_cursor_theme_list(char ***names);

/* ---------- Icon themes ---------- */

/* List available icon theme names (from index.theme Name= field).
 * Scans $XDG_DATA_DIRS/icons/{theme}/index.theme. */
int isde_icon_theme_list(char ***names);

/* Look up an icon path in a theme by name and category.
 * Prefers scalable SVGs. Returns malloc'd path or NULL. */
char *isde_icon_theme_lookup(const char *theme, const char *category,
                              const char *icon_name);

/* ---------- Global active theme ---------- */

/* Load and cache the active colour scheme from isde.toml [appearance].
 * Returns the cached scheme (do not free). Returns NULL if no scheme configured. */
const IsdeColorScheme *isde_theme_current(void);

/* Reload the active colour scheme from config (call after D-Bus notification). */
void isde_theme_reload(void);

/* Convert a theme color (0xRRGGBB) to a double triplet (0.0-1.0). */
void isde_color_to_rgb(unsigned int color, double *r, double *g, double *b);

/* Build an Xt fallback resource list from the current colour scheme.
 * Returns a NULL-terminated array of strings suitable for
 * XtAppInitialize()'s fallback_resources parameter.
 * Caller must free with isde_theme_free_resources(). */
char **isde_theme_build_resources(void);
void   isde_theme_free_resources(char **resources);

#ifdef __cplusplus
}
#endif

#endif /* ISDE_THEME_H */
