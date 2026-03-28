/*
 * isde-mime.h — MIME type detection from file extensions
 *
 * Uses the shared-mime-info globs2 database.
 */
#ifndef ISDE_MIME_H
#define ISDE_MIME_H

#ifdef __cplusplus
extern "C" {
#endif

/* Return the MIME type for a filename based on its extension.
 * Looks up /usr/share/mime/globs2 (cached after first call).
 * Returns "application/octet-stream" if no match is found.
 * The returned string is owned by the cache — do not free. */
const char *isde_mime_type_for_file(const char *filename);

/* Find the default .desktop file ID for a MIME type.
 * Reads mimeapps.list files in freedesktop priority order:
 *   1. ~/.config/mimeapps.list
 *   2. /usr/share/applications/mimeapps.list
 *   3. /usr/share/applications/defaults.list
 * Returns a malloc'd desktop file ID (e.g. "firefox.desktop") or NULL.
 * Caller must free. */
char *isde_mime_default_app(const char *mime);

/* Resolve a .desktop file ID to its full path.
 * Searches ~/.local/share/applications/ and /usr/share/applications/.
 * Returns a malloc'd path or NULL.  Caller must free. */
char *isde_mime_find_desktop(const char *desktop_id);

/* Set the default application for a MIME type.
 * Writes to ~/.config/mimeapps.list.
 * desktop_id is a .desktop filename (e.g. "firefox.desktop").
 * Returns 0 on success, -1 on error. */
int isde_mime_set_default(const char *mime, const char *desktop_id);

#ifdef __cplusplus
}
#endif

#endif /* ISDE_MIME_H */
