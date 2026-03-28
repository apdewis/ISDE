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

/* ---------- Colour scheme ---------- */

/* Widget state colours — background, foreground, border, hover variants */
typedef struct {
    unsigned int bg;
    unsigned int fg;
    unsigned int border;
    unsigned int hover_bg;
    unsigned int hover_fg;
} IsdeElementColors;

typedef struct {
    char    *name;
    char    *author;

    /* Global defaults */
    unsigned int bg;            /* default background */
    unsigned int bg_light;      /* lighter background (panels, buttons) */
    unsigned int bg_bright;     /* bright background (text inputs) */
    unsigned int fg;            /* default foreground */
    unsigned int fg_dim;        /* secondary/dimmed foreground */
    unsigned int fg_light;      /* light foreground (on dark backgrounds) */
    unsigned int border;        /* borders, separators */
    unsigned int select_bg;     /* selection background */
    unsigned int select_fg;     /* selection foreground */

    /* Semantic status colours */
    unsigned int error;         /* errors, destructive actions */
    unsigned int warning;       /* warnings, caution */
    unsigned int success;       /* confirmation, positive state */
    unsigned int active;        /* focused elements, links, primary */
    unsigned int accent;        /* decorative emphasis */

    /* Element-specific overrides */
    IsdeElementColors titlebar;         /* title bar (unfocused) */
    IsdeElementColors titlebar_active;  /* title bar (focused) */
    IsdeElementColors titlebar_button;  /* title bar min/max buttons */
    IsdeElementColors close_button;     /* title bar close button */
    IsdeElementColors menu;             /* dropdown/popup menus */
    IsdeElementColors menu_item;        /* menu items */
    IsdeElementColors taskbar;              /* taskbar bar background */
    IsdeElementColors taskbar_button;       /* taskbar button (no windows) */
    IsdeElementColors taskbar_button_active; /* taskbar button (has windows) */
    IsdeElementColors taskbar_button_focus;  /* taskbar button (focused app) */
} IsdeColorScheme;

/* Load a named colour scheme from the theme directories.
 * Returns NULL if not found. Caller must free with isde_scheme_free(). */
IsdeColorScheme *isde_scheme_load(const char *name);
void             isde_scheme_free(IsdeColorScheme *scheme);

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

/* Compute a widget height (in scaled pixels) suitable for containing text
 * rendered in the given font category.  Reads [fonts] from isde.toml.
 * category: "general", "fixed", "small", "toolbar", "menu", "title"
 * padding:  extra vertical padding (unscaled pixels) added to font height */
int isde_font_height(const char *category, int padding);

#ifdef __cplusplus
}
#endif

#endif /* ISDE_THEME_H */
