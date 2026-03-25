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

/* Find an ISDE icon by category and name.
 * Searches: $XDG_DATA_HOME/isde/icons/<category>/<name>.svg
 *           $XDG_DATA_DIRS/isde/icons/<category>/<name>.svg
 *           <source_tree>/common/data/icons/<category>/<name>.svg
 * Returns the first path that exists (caller must free), or NULL. */
char *isde_icon_find(const char *category, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* ISDE_XDG_H */
