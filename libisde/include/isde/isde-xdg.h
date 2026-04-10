/*
 * isde-xdg.h — XDG Base Directory Specification helpers
 */
#ifndef ISDE_XDG_H
#define ISDE_XDG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Return the XDG config home directory (~/.config by default).
 * The returned string is owned by libisde — do not free. */
const char *isde_xdg_config_home(void);

/* Return the XDG data home directory (~/.local/share by default).
 * The returned string is owned by libisde — do not free. */
const char *isde_xdg_data_home(void);

/* Return the XDG cache home directory (~/.cache by default).
 * The returned string is owned by libisde — do not free. */
const char *isde_xdg_cache_home(void);

/* Return a colon-separated list of XDG config directories
 * (/etc/xdg by default).  Owned by libisde. */
const char *isde_xdg_config_dirs(void);

/* Return a colon-separated list of XDG data directories
 * (/usr/local/share:/usr/share by default).  Owned by libisde. */
const char *isde_xdg_data_dirs(void);

/* Build a path: <xdg_config_home>/isde/<suffix>.
 * Caller must free() the returned string. Returns NULL on allocation failure. */
char *isde_xdg_config_path(const char *suffix);

/* Build a path: <xdg_data_home>/isde/<suffix>.
 * Caller must free() the returned string. */
char *isde_xdg_data_path(const char *suffix);

/* Search for a file named <name> in XDG config directories:
 *   $XDG_CONFIG_HOME/isde/<name>  then  each $XDG_CONFIG_DIRS/isde/<name>
 * Returns the first path that exists (caller must free), or NULL. */
char *isde_xdg_find_config(const char *name);

/* Search for a file in XDG data directories (same logic as above). */
char *isde_xdg_find_data(const char *name);

/* Find an icon by category and name.
 * Searches the configured icon theme (default: isde-standard) via the
 * freedesktop theme lookup chain (theme -> Inherits -> hicolor).  If the
 * configured theme is not isde-standard, isde-standard is tried as a final
 * fallback so ISDE's own UI icons are always resolvable.
 * Returns the first path that exists (caller must free), or NULL. */
char *isde_icon_find(const char *category, const char *name);

/* Return the path for an XDG user directory.
 * Parses $XDG_CONFIG_HOME/user-dirs.dirs.
 * Valid names: DESKTOP, DOCUMENTS, DOWNLOAD, MUSIC, PICTURES,
 *              PUBLICSHARE, TEMPLATES, VIDEOS.
 * Returns a malloc'd path, or NULL if the directory is not configured
 * or is set to $HOME.  Caller must free(). */
char *isde_xdg_user_dir(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* ISDE_XDG_H */
