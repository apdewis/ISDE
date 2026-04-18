/*
 * isde-dialog.h — HIG-compliant dialog helpers and standard dialogs
 *
 * Provides reusable infrastructure for creating modal/modeless dialogs
 * with correct shell setup, button layout, and dismiss handling.
 * Also provides ready-made standard dialogs: message, confirm, input,
 * font chooser, about, and progress.
 *
 * All standard dialogs are callback-based (Xt has no nested event loop).
 * Each invocation creates and destroys its own shell — no global state.
 */
#ifndef ISDE_DIALOG_H
#define ISDE_DIALOG_H

#include <ISW/Intrinsic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Core helpers — extract duplicated dialog boilerplate
 * ================================================================ */

/*
 * Create a transient popup shell with:
 *   - WM_DELETE_WINDOW → dismiss
 *   - Escape key → dismiss
 *   - ISW auto-scales width/height for HiDPI
 *   - borderWidth = 1
 *   - transient-for = nearest Shell ancestor of parent
 *
 * The caller adds content widgets, then calls isde_dialog_popup().
 */
Widget isde_dialog_create_shell(Widget parent, const char *name,
                                const char *title, int width, int height);

/* Pop up the dialog.  IswGrabExclusive for modal, IswGrabNone for modeless. */
void isde_dialog_popup(Widget shell, IswGrabKind grab);

/* Pop down and destroy the dialog shell.  Safe to call with NULL. */
void isde_dialog_dismiss(Widget shell);

/*
 * Create a row of HIG-compliant buttons inside a Form widget:
 *   - Each button: 80 logical pixels wide, 8px internal padding
 *   - Anchored bottom-right (IswChainRight, IswChainBottom)
 *   - Affirmative action first (leftmost), Cancel last (rightmost)
 *
 * form:         the Form widget to add buttons to
 * above_widget: the widget buttons are placed below (IswNfromVert)
 * form_width:   scaled width of the form (for horizDistance calculation)
 * buttons:      array of button specifications
 * nbuttons:     number of buttons
 *
 * Returns the leftmost button widget.
 */
typedef struct {
    const char    *label;
    IswCallbackProc callback;
    IswPointer      client_data;
} IsdeDialogButton;

Widget isde_dialog_add_buttons(Widget form, Widget above_widget,
                               int form_width,
                               const IsdeDialogButton *buttons, int nbuttons);

/* ================================================================
 * Standard dialogs
 * ================================================================ */

typedef enum {
    ISDE_DIALOG_OK = 0,
    ISDE_DIALOG_CANCEL,
} IsdeDialogResult;

typedef void (*IsdeDialogResultCB)(IsdeDialogResult result, void *data);
typedef void (*IsdeDialogInputCB)(IsdeDialogResult result,
                                  const char *text, void *data);

/*
 * All standard dialog functions return the popup shell widget.
 * Callers who need to dismiss a previous instance before opening
 * a new one can call isde_dialog_dismiss() on the stored shell.
 * Callers who don't need this can ignore the return value.
 */

/* Message box — informational, single OK button, modal. */
Widget isde_dialog_message(Widget parent, const char *title,
                           const char *message,
                           IsdeDialogResultCB callback, void *data);

/* Confirmation — message + action button + Cancel, modal.
 * action_label: text for the affirmative button (e.g. "Delete").
 * Callback receives ISDE_DIALOG_OK or ISDE_DIALOG_CANCEL. */
Widget isde_dialog_confirm(Widget parent, const char *title,
                           const char *message, const char *action_label,
                           IsdeDialogResultCB callback, void *data);

/* Input — label + text field + OK/Cancel, modal.
 * initial_value: pre-filled text (may be NULL).
 * Callback receives ISDE_DIALOG_OK with the entered text, or CANCEL. */
Widget isde_dialog_input(Widget parent, const char *title,
                         const char *prompt, const char *initial_value,
                         IsdeDialogInputCB callback, void *data);

/* Font chooser — wraps ISW FontChooser + OK/Cancel, modal.
 * Callback receives the chosen family + size on OK, or NULL on cancel. */
typedef void (*IsdeDialogFontCB)(IsdeDialogResult result,
                                 const char *family, int size, void *data);

Widget isde_dialog_font(Widget parent, const char *title,
                        const char *initial_family, int initial_size,
                        IsdeDialogFontCB callback, void *data);

/* About dialog — app name, version, description, single Close button.
 * icon_path may be NULL. */
Widget isde_dialog_about(Widget parent, const char *app_name,
                         const char *version, const char *description,
                         const char *icon_path);

/* Progress dialog — non-modal, delayed show (500ms).
 * Returns an opaque handle for updating/dismissing. */
typedef struct IsdeProgress IsdeProgress;

IsdeProgress *isde_progress_create(Widget parent, const char *title,
                                   IswAppContext app,
                                   IswCallbackProc cancel_cb, void *data);
void isde_progress_update(IsdeProgress *p, int percent, const char *message);
void isde_progress_update_file(IsdeProgress *p, int percent,
                               const char *message);
void isde_progress_destroy(IsdeProgress *p);

#ifdef __cplusplus
}
#endif

#endif /* ISDE_DIALOG_H */
