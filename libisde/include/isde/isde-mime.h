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

#ifdef __cplusplus
}
#endif

#endif /* ISDE_MIME_H */
