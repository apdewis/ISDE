/*
 * isde-desktop.h — freedesktop.org .desktop file parser
 *
 * Parses the [Desktop Entry] group per the Desktop Entry Specification.
 */
#ifndef ISDE_DESKTOP_H
#define ISDE_DESKTOP_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IsdeDesktopEntry IsdeDesktopEntry;

typedef struct IsdeDesktopAction {
    const char *id;     /* Action identifier (e.g. "new-window") */
    const char *name;   /* Display name */
    const char *exec;   /* Command to execute */
    const char *icon;   /* Optional icon (may be NULL) */
} IsdeDesktopAction;

/* Parse a .desktop file.  Returns NULL on failure. */
IsdeDesktopEntry *isde_desktop_load(const char *path);
void              isde_desktop_free(IsdeDesktopEntry *entry);

/* Field accessors — return NULL / 0 if the field is absent.
 * Returned strings are owned by the entry; do not free. */
const char *isde_desktop_name(const IsdeDesktopEntry *e);
const char *isde_desktop_generic_name(const IsdeDesktopEntry *e);
const char *isde_desktop_comment(const IsdeDesktopEntry *e);
const char *isde_desktop_exec(const IsdeDesktopEntry *e);
const char *isde_desktop_icon(const IsdeDesktopEntry *e);
const char *isde_desktop_type(const IsdeDesktopEntry *e);
const char *isde_desktop_categories(const IsdeDesktopEntry *e);
int         isde_desktop_terminal(const IsdeDesktopEntry *e);
int         isde_desktop_no_display(const IsdeDesktopEntry *e);
int         isde_desktop_hidden(const IsdeDesktopEntry *e);

/* Desktop Actions (e.g. "New Window", "New Private Window").
 * Returns the number of actions. */
int isde_desktop_action_count(const IsdeDesktopEntry *e);
const IsdeDesktopAction *isde_desktop_action(const IsdeDesktopEntry *e, int index);

/* Check OnlyShowIn / NotShowIn against a desktop name (e.g. "ISDE").
 * Returns 1 if the entry should be shown, 0 if filtered out. */
int isde_desktop_should_show(const IsdeDesktopEntry *e, const char *desktop);

/* Build the actual command line from the Exec field, substituting
 * field codes (%f, %F, %u, %U, etc.) with the given file/URL list.
 * files may be NULL if no arguments.  Caller must free() the result. */
char *isde_desktop_build_exec(const IsdeDesktopEntry *e,
                              const char **files, int nfiles);

/* Scan a directory for all .desktop files.  Returns an allocated array
 * of IsdeDesktopEntry pointers (caller must free the array and each entry).
 * *count is set to the number of entries found. */
IsdeDesktopEntry **isde_desktop_scan_dir(const char *dirpath, int *count);

#ifdef __cplusplus
}
#endif

#endif /* ISDE_DESKTOP_H */
