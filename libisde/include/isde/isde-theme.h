/*
 * isde-theme.h — theme scanning and colour scheme loading
 *
 * Colour schemes: .theme INI files in $XDG_DATA_DIRS/isde/themes/
 * Cursor themes: freedesktop Xcursor in $XDG_DATA_DIRS/icons/{theme}/cursors/
 * Icon themes: freedesktop Icon Theme Spec in $XDG_DATA_DIRS/icons/{theme}/index.theme
 */
#ifndef ISDE_THEME_H
#define ISDE_THEME_H

#include <ISW/Intrinsic.h>

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

    /* Terminal palette — 16 ANSI + foreground + background + cursor.
     * Values are 0xRRGGBB. If terminal_valid is 0 the palette was not
     * explicitly provided by the theme and callers should fall back to
     * defaults derived from bg/fg. */
    int          terminal_valid;
    unsigned int terminal_ansi[16];
    unsigned int terminal_fg;
    unsigned int terminal_bg;
    unsigned int terminal_cursor;
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

/* Return the configured cursor theme name from isde.toml [appearance].
 * Returns NULL if not set. The returned string is static; do not free. */
const char *isde_cursor_theme_configured(void);

/* Return the configured cursor size as a string from isde.toml [appearance].
 * Returns NULL if not set. The returned string is static; do not free. */
const char *isde_cursor_size_configured(void);

/* ---------- Icon themes ---------- */

/* List available icon theme names (from index.theme Name= field).
 * Scans $XDG_DATA_DIRS/icons/{theme}/index.theme. */
int isde_icon_theme_list(char ***names);

/* List available icon themes, returning both display names (from
 * index.theme Name=) and the theme's directory name (the XDG identifier
 * used in config files and passed to isde_icon_theme_lookup). Both
 * arrays are parallel and of equal length; caller frees each string
 * and both arrays. */
int isde_icon_theme_list_full(char ***display_names, char ***dir_names);

/* Look up an icon path in a theme by name and category.
 * Prefers scalable SVGs. Returns malloc'd path or NULL. */
char *isde_icon_theme_lookup(const char *theme, const char *category,
                              const char *icon_name);

/* ---------- Global active theme ---------- */

/* Load and cache the active colour scheme from isde.toml [appearance].
 * Returns the cached scheme (do not free). Returns NULL if no scheme configured. */
const IsdeColorScheme *isde_theme_current(void);

/* Override the active colour scheme.  Takes ownership of the scheme
 * (it will be freed on reload or on a subsequent set call).
 * Call before isde_theme_build_resources() to force a specific scheme
 * instead of reading isde.toml — useful for the greeter. */
void isde_theme_set_scheme(IsdeColorScheme *scheme);

/* Reload the active colour scheme from config (call after D-Bus notification). */
void isde_theme_reload(void);

/* Convert a theme color (0xRRGGBB) to a double triplet (0.0-1.0). */
void isde_color_to_rgb(unsigned int color, double *r, double *g, double *b);

/* Merge the current colour scheme into the per-screen Xrm database
 * attached to the given toplevel widget.  Call after IswAppInitialize()
 * and before creating child widgets. */
void isde_theme_merge_xrm(Widget toplevel);

/* Put a single "resource: value" line into the per-screen Xrm database. */
void isde_xrm_put_line(Widget toplevel, const char *line);

/* Build an Xt resource list from the current colour scheme.
 * Returns a NULL-terminated array of strings.
 * Caller must free with isde_theme_free_resources().
 * Prefer isde_theme_merge_xrm() which calls this internally. */
char **isde_theme_build_resources(void);
void   isde_theme_free_resources(char **resources);

/* Compute a widget height (in scaled pixels) suitable for containing text
 * rendered in the given font category.  Reads [fonts] from isde.toml.
 * category: "general", "fixed", "small", "toolbar", "menu", "title"
 * padding:  extra vertical padding (unscaled pixels) added to font height */
int isde_font_height(const char *category, int padding);

/* Resolve a fontconfig-style "Family-Size" spec to an IswFontStruct* via
 * ISW's String→FontStruct converter.  Returns NULL on failure.  The returned
 * struct is owned by the converter cache and must not be freed. */
struct _IswFontStruct *isde_resolve_font(Widget w, const char *spec);

#ifdef __cplusplus
}
#endif

#endif /* ISDE_THEME_H */
