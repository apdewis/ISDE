/*
 * isde-filechooser.h — File open/save dialog for ISDE
 *
 * Two-pane layout: directory list (left) + file list (right).
 * Uses ISW List widgets in Viewports, POSIX opendir/readdir for
 * directory scanning, and isde_dialog helpers for HIG compliance.
 */
#ifndef ISDE_FILECHOOSER_H
#define ISDE_FILECHOOSER_H

#include <X11/Intrinsic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ISDE_FILE_OPEN,
    ISDE_FILE_SAVE,
} IsdeFileChooserMode;

/*
 * Callback receives the selected path on OK, NULL on cancel.
 * For SAVE mode, the path may not yet exist.
 */
typedef void (*IsdeFileChooserCB)(const char *path, void *data);

/*
 * Show a file chooser dialog.
 *
 * parent:      any widget (nearest shell ancestor used for transient-for)
 * title:       window title (e.g. "Open File", "Save As")
 * mode:        ISDE_FILE_OPEN or ISDE_FILE_SAVE
 * initial_dir: starting directory (NULL = home directory)
 * filter:      glob pattern for file filtering (NULL = show all,
 *              e.g. "*.png", "*.txt")
 * callback:    called with selected path or NULL on cancel
 * data:        user data for callback
 *
 * Returns the popup shell widget (for tracking/dismissal).
 * The dialog is modal (XtGrabExclusive).
 */
Widget isde_filechooser_show(Widget parent, const char *title,
                             IsdeFileChooserMode mode,
                             const char *initial_dir,
                             const char *filter,
                             IsdeFileChooserCB callback, void *data);

#ifdef __cplusplus
}
#endif

#endif /* ISDE_FILECHOOSER_H */
