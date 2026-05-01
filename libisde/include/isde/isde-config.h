/*
 * isde-config.h — TOML configuration file loading
 *
 * Wraps tomlc99 with XDG-aware file resolution. Config files are
 * looked up in $XDG_CONFIG_HOME/isde/ with fallback to $XDG_CONFIG_DIRS.
 */
#ifndef ISDE_CONFIG_H
#define ISDE_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IsdeConfig IsdeConfig;

/* Parse a TOML file at the given path.  Returns NULL on error;
 * if errbuf is non-NULL, a human-readable message is written there. */
IsdeConfig *isde_config_load(const char *path, char *errbuf, int errbufsz);

/* Load a named config file (e.g. "isde.toml") by searching XDG config
 * dirs: $XDG_CONFIG_HOME/isde/<name>, then each $XDG_CONFIG_DIRS/isde/<name>.
 * Returns NULL if not found or parse error. */
IsdeConfig *isde_config_load_xdg(const char *name, char *errbuf, int errbufsz);

void isde_config_free(IsdeConfig *cfg);

/* Table navigation — returns an opaque handle to a [section].
 * Pass NULL as section to operate on the root table. */
typedef struct IsdeConfigTable IsdeConfigTable;

IsdeConfigTable *isde_config_root(IsdeConfig *cfg);
IsdeConfigTable *isde_config_table(IsdeConfigTable *parent, const char *key);

/* Value accessors — return the value if found, or the provided default. */
const char *isde_config_string(IsdeConfigTable *tbl, const char *key,
                               const char *def);
int64_t     isde_config_int(IsdeConfigTable *tbl, const char *key, int64_t def);
int         isde_config_bool(IsdeConfigTable *tbl, const char *key, int def);
double      isde_config_double(IsdeConfigTable *tbl, const char *key, double def);

/* Array-of-tables iteration (e.g. [[keybind]]).
 * Returns the number of entries; use isde_config_array_table() to get each. */
int              isde_config_array_count(IsdeConfigTable *tbl, const char *key);
IsdeConfigTable *isde_config_array_table(IsdeConfigTable *tbl, const char *key,
                                         int index);

/* String array (e.g. tags = ["a", "b"]).
 * Returns count; fills out[] with pointers valid until config is freed.
 * Caller must free each string with free(). */
int isde_config_string_array(IsdeConfigTable *tbl, const char *key,
                             char ***out);

/* ---------- Global ISDE settings (from isde.toml) ---------- */

/* Return the double-click threshold in milliseconds.
 * Reads [input].double_click_ms from isde.toml, defaults to 400. */
int isde_config_double_click_ms(void);

/* Clear all cached global settings so the next accessor re-reads from file. */
void isde_config_invalidate_cache(void);

/* ---------- Config file writing ---------- */

/* Write a value into a TOML file. Creates the file/section if missing.
 * path: file path (e.g. from isde_xdg_config_path("isde.toml"))
 * section: TOML table name (e.g. "input")
 * key: key name (e.g. "double_click_ms")
 * Returns 0 on success. */
int isde_config_write_string(const char *path, const char *section,
                              const char *key, const char *value);
int isde_config_write_int(const char *path, const char *section,
                           const char *key, int64_t value);
int isde_config_write_bool(const char *path, const char *section,
                            const char *key, int value);
int isde_config_write_double(const char *path, const char *section,
                              const char *key, double value);

#ifdef __cplusplus
}
#endif

#endif /* ISDE_CONFIG_H */
