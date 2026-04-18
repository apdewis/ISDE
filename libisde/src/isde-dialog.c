#define _POSIX_C_SOURCE 200809L
/*
 * isde-dialog.c — HIG-compliant dialog helpers and standard dialogs
 */
#include "isde/isde-dialog.h"
#include "isde/isde-xdg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>
#include <ISW/StringDefs.h>
#include <ISW/Shell.h>
#include <ISW/Command.h>
#include <ISW/Label.h>
#include <ISW/Form.h>
#include <ISW/FlexBox.h>
#include <ISW/Dialog.h>
#include <ISW/FontChooser.h>
#include <ISW/ProgressBar.h>

/* ================================================================
 * Internal: Xt action for dismiss (registered once)
 * ================================================================ */

static void act_isde_dialog_dismiss(Widget w, xcb_generic_event_t *ev,
                                    String *params, Cardinal *nparams)
{
    (void)ev; (void)params; (void)nparams;
    /* Walk up to the shell */
    while (w && !IswIsShell(w))
        w = IswParent(w);
    isde_dialog_dismiss(w);
}

static IswActionsRec dialog_actions[] = {
    {"isde-dialog-dismiss", act_isde_dialog_dismiss},
};

static void ensure_actions_registered(Widget any_widget)
{
    static int registered;
    if (!registered) {
        IswAppAddActions(IswWidgetToApplicationContext(any_widget),
                        dialog_actions, IswNumber(dialog_actions));
        registered = 1;
    }
}

/* Find nearest Shell ancestor (or w itself if it is a shell) */
static Widget find_shell_ancestor(Widget w)
{
    while (w && !IswIsShell(w))
        w = IswParent(w);
    return w;
}

/* ================================================================
 * Core helpers
 * ================================================================ */

Widget isde_dialog_create_shell(Widget parent, const char *name,
                                const char *title, int width, int height)
{
    ensure_actions_registered(parent);

    Widget shell_parent = find_shell_ancestor(parent);
    if (!shell_parent)
        shell_parent = parent;

    Arg args[20];
    Cardinal n = 0;
    IswSetArg(args[n], IswNwidth, width);   n++;
    IswSetArg(args[n], IswNheight, height);  n++;
    IswSetArg(args[n], IswNborderWidth, 1);              n++;
    if (title) {
        IswSetArg(args[n], IswNtitle, title);            n++;
    }
    Widget shell = IswCreatePopupShell((String)name, transientShellWidgetClass,
                                      shell_parent, args, n);

    /* Escape and WM close button both dismiss */
    IswOverrideTranslations(shell, IswParseTranslationTable(
        "<Message>WM_PROTOCOLS: isde-dialog-dismiss()\n"
        "<Key>Escape: isde-dialog-dismiss()\n"));

    return shell;
}

void isde_dialog_popup(Widget shell, IswGrabKind grab)
{
    if (shell)
        IswPopup(shell, grab);
}

void isde_dialog_dismiss(Widget shell)
{
    if (!shell)
        return;
    IswPopdown(shell);
    IswDestroyWidget(shell);
}

Widget isde_dialog_add_buttons(Widget form, Widget above_widget,
                               int form_width,
                               const IsdeDialogButton *buttons, int nbuttons)
{
    if (nbuttons <= 0)
        return NULL;

    int btn_w   = 80;
    int btn_pad = 8;

    /* First button horizDistance: push buttons to the right edge */
    int total_btn_width = btn_w * nbuttons + btn_pad * (nbuttons - 1);
    int first_horiz = form_width - total_btn_width - btn_pad;
    if (first_horiz < 0)
        first_horiz = 0;

    Widget first = NULL;
    Widget prev = NULL;

    for (int i = 0; i < nbuttons; i++) {
        Arg args[20];
        Cardinal n = 0;
        IswSetArg(args[n], IswNlabel, buttons[i].label);      n++;
        IswSetArg(args[n], IswNwidth, btn_w);                  n++;
        IswSetArg(args[n], IswNinternalWidth, btn_pad);        n++;
        IswSetArg(args[n], IswNinternalHeight, btn_pad);       n++;
        if (above_widget) {
            IswSetArg(args[n], IswNfromVert, above_widget);    n++;
        }
        if (i == 0) {
            IswSetArg(args[n], IswNhorizDistance, first_horiz); n++;
        } else {
            IswSetArg(args[n], IswNfromHoriz, prev);           n++;
            IswSetArg(args[n], IswNhorizDistance, btn_pad);    n++;
        }
        IswSetArg(args[n], IswNleft, IswChainRight);            n++;
        IswSetArg(args[n], IswNright, IswChainRight);           n++;
        IswSetArg(args[n], IswNbottom, IswChainBottom);         n++;
        IswSetArg(args[n], IswNtop, IswChainBottom);            n++;
        Widget btn = IswCreateManagedWidget("btn", commandWidgetClass,
                                           form, args, n);
        IswAddCallback(btn, IswNcallback, buttons[i].callback,
                      buttons[i].client_data);
        if (i == 0) first = btn;
        prev = btn;
    }

    return first;
}

/* ================================================================
 * Internal context for standard dialogs
 * ================================================================ */

typedef struct {
    Widget               shell;
    Widget               dialog_widget;  /* ISW Dialog (for input dialogs) */
    Widget               chooser_widget; /* ISW FontChooser */
    IsdeDialogResultCB   result_cb;
    IsdeDialogInputCB    input_cb;
    IsdeDialogFontCB     font_cb;
    void                *user_data;
} DialogCtx;

static DialogCtx *ctx_new(void)
{
    DialogCtx *ctx = calloc(1, sizeof(*ctx));
    return ctx;
}

static void ctx_dismiss_and_free(DialogCtx *ctx)
{
    if (!ctx) return;
    Widget shell = ctx->shell;
    ctx->shell = NULL;
    free(ctx);
    isde_dialog_dismiss(shell);
}

/* ================================================================
 * Confirm dialog
 * ================================================================ */

static void confirm_action_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    DialogCtx *ctx = (DialogCtx *)cd;
    if (ctx->result_cb)
        ctx->result_cb(ISDE_DIALOG_OK, ctx->user_data);
    ctx_dismiss_and_free(ctx);
}

static void confirm_cancel_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    DialogCtx *ctx = (DialogCtx *)cd;
    if (ctx->result_cb)
        ctx->result_cb(ISDE_DIALOG_CANCEL, ctx->user_data);
    ctx_dismiss_and_free(ctx);
}

Widget isde_dialog_confirm(Widget parent, const char *title,
                           const char *message, const char *action_label,
                           IsdeDialogResultCB callback, void *data)
{
    DialogCtx *ctx = ctx_new();
    ctx->result_cb = callback;
    ctx->user_data = data;

    ctx->shell = isde_dialog_create_shell(parent, "confirmShell",
                                          title, 300, 120);

    Arg args[20];
    Cardinal n = 0;
    IswSetArg(args[n], IswNlabel, message); n++;
    Widget dialog = IswCreateManagedWidget("confirmDialog", dialogWidgetClass,
                                          ctx->shell, args, n);

    Widget anchor = IswNameToWidget(dialog, "label");

    IsdeDialogButton btns[2] = {
        { action_label, confirm_action_cb, ctx },
        { "Cancel",     confirm_cancel_cb, ctx },
    };
    isde_dialog_add_buttons(dialog, anchor, 300, btns, 2);

    isde_dialog_popup(ctx->shell, IswGrabExclusive);
    return ctx->shell;
}

/* ================================================================
 * Message dialog
 * ================================================================ */

static void message_ok_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    DialogCtx *ctx = (DialogCtx *)cd;
    if (ctx->result_cb)
        ctx->result_cb(ISDE_DIALOG_OK, ctx->user_data);
    ctx_dismiss_and_free(ctx);
}

Widget isde_dialog_message(Widget parent, const char *title,
                           const char *message,
                           IsdeDialogResultCB callback, void *data)
{
    DialogCtx *ctx = ctx_new();
    ctx->result_cb = callback;
    ctx->user_data = data;

    ctx->shell = isde_dialog_create_shell(parent, "messageShell",
                                          title, 300, 120);

    Arg args[20];
    Cardinal n = 0;
    IswSetArg(args[n], IswNlabel, message); n++;
    Widget dialog = IswCreateManagedWidget("messageDialog", dialogWidgetClass,
                                          ctx->shell, args, n);

    Widget anchor = IswNameToWidget(dialog, "label");

    IsdeDialogButton btns[1] = {
        { "OK", message_ok_cb, ctx },
    };
    isde_dialog_add_buttons(dialog, anchor, 300, btns, 1);

    isde_dialog_popup(ctx->shell, IswGrabExclusive);
    return ctx->shell;
}

/* ================================================================
 * Input dialog
 * ================================================================ */

static void input_ok_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    DialogCtx *ctx = (DialogCtx *)cd;
    char *text = NULL;
    if (ctx->dialog_widget)
        text = IswDialogGetValueString(ctx->dialog_widget);
    if (ctx->input_cb)
        ctx->input_cb(ISDE_DIALOG_OK, text, ctx->user_data);
    ctx_dismiss_and_free(ctx);
}

static void input_cancel_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    DialogCtx *ctx = (DialogCtx *)cd;
    if (ctx->input_cb)
        ctx->input_cb(ISDE_DIALOG_CANCEL, NULL, ctx->user_data);
    ctx_dismiss_and_free(ctx);
}

Widget isde_dialog_input(Widget parent, const char *title,
                         const char *prompt, const char *initial_value,
                         IsdeDialogInputCB callback, void *data)
{
    DialogCtx *ctx = ctx_new();
    ctx->input_cb = callback;
    ctx->user_data = data;

    ctx->shell = isde_dialog_create_shell(parent, "inputShell",
                                          title, 300, 120);

    Arg args[20];
    Cardinal n = 0;
    IswSetArg(args[n], IswNlabel, prompt);                    n++;
    if (initial_value) {
        IswSetArg(args[n], IswNvalue, initial_value);         n++;
    }
    ctx->dialog_widget = IswCreateManagedWidget("inputDialog",
                                               dialogWidgetClass,
                                               ctx->shell, args, n);

    Widget value_w = IswNameToWidget(ctx->dialog_widget, "value");
    Widget anchor = value_w ? value_w :
                    IswNameToWidget(ctx->dialog_widget, "label");

    IsdeDialogButton btns[2] = {
        { "OK",     input_ok_cb,     ctx },
        { "Cancel", input_cancel_cb, ctx },
    };
    isde_dialog_add_buttons(ctx->dialog_widget, anchor,
                            300, btns, 2);

    isde_dialog_popup(ctx->shell, IswGrabExclusive);
    return ctx->shell;
}

/* ================================================================
 * Font chooser dialog
 * ================================================================ */

static void font_ok_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    DialogCtx *ctx = (DialogCtx *)cd;
    const char *family = NULL;
    int size = 0;
    if (ctx->chooser_widget) {
        family = IswFontChooserGetFamily(ctx->chooser_widget);
        size = IswFontChooserGetSize(ctx->chooser_widget);
    }
    if (ctx->font_cb)
        ctx->font_cb(ISDE_DIALOG_OK, family, size, ctx->user_data);
    ctx_dismiss_and_free(ctx);
}

static void font_cancel_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    DialogCtx *ctx = (DialogCtx *)cd;
    if (ctx->font_cb)
        ctx->font_cb(ISDE_DIALOG_CANCEL, NULL, 0, ctx->user_data);
    ctx_dismiss_and_free(ctx);
}

Widget isde_dialog_font(Widget parent, const char *title,
                        const char *initial_family, int initial_size,
                        IsdeDialogFontCB callback, void *data)
{
    DialogCtx *ctx = ctx_new();
    ctx->font_cb = callback;
    ctx->user_data = data;

    ctx->shell = isde_dialog_create_shell(parent, "fontChooserShell",
                                          title, 400, 350);

    Arg args[20];
    Cardinal n = 0;
    IswSetArg(args[n], IswNdefaultDistance, 8); n++;
    IswSetArg(args[n], IswNborderWidth, 0);                 n++;
    Widget form = IswCreateManagedWidget("fcForm", formWidgetClass,
                                        ctx->shell, args, n);

    /* FontChooser widget */
    n = 0;
    if (initial_family) {
        IswSetArg(args[n], IswNfontFamily, initial_family); n++;
    }
    IswSetArg(args[n], IswNfontSize, initial_size);          n++;
    IswSetArg(args[n], IswNborderWidth, 0);                  n++;
    IswSetArg(args[n], IswNtop, IswChainTop);                 n++;
    IswSetArg(args[n], IswNbottom, IswChainBottom);           n++;
    IswSetArg(args[n], IswNleft, IswChainLeft);               n++;
    IswSetArg(args[n], IswNright, IswChainRight);             n++;
    IswSetArg(args[n], IswNwidth, 390);          n++;
    IswSetArg(args[n], IswNheight, 290);         n++;
    ctx->chooser_widget = IswCreateManagedWidget("fontChooser",
                                                 fontChooserWidgetClass,
                                                 form, args, n);

    IsdeDialogButton btns[2] = {
        { "OK",     font_ok_cb,     ctx },
        { "Cancel", font_cancel_cb, ctx },
    };
    isde_dialog_add_buttons(form, ctx->chooser_widget,
                            400 - 8 * 2, btns, 2);

    isde_dialog_popup(ctx->shell, IswGrabExclusive);
    return ctx->shell;
}

/* ================================================================
 * About dialog
 * ================================================================ */

static void about_close_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    DialogCtx *ctx = (DialogCtx *)cd;
    ctx_dismiss_and_free(ctx);
}

Widget isde_dialog_about(Widget parent, const char *app_name,
                         const char *version, const char *description,
                         const char *icon_path)
{
    DialogCtx *ctx = ctx_new();

    ctx->shell = isde_dialog_create_shell(parent, "aboutShell",
                                          app_name, 300, 200);

    Arg args[20];
    Cardinal n = 0;
    IswSetArg(args[n], IswNdefaultDistance, 8); n++;
    IswSetArg(args[n], IswNborderWidth, 0);                 n++;
    Widget form = IswCreateManagedWidget("aboutForm", formWidgetClass,
                                        ctx->shell, args, n);

    Widget prev = NULL;

    /* Icon (optional) */
    if (icon_path) {
        n = 0;
        IswSetArg(args[n], IswNimage, icon_path);        n++;
        IswSetArg(args[n], IswNlabel, "");               n++;
        IswSetArg(args[n], IswNborderWidth, 0);          n++;
        prev = IswCreateManagedWidget("aboutIcon", labelWidgetClass,
                                      form, args, n);
    }

    /* App name + version */
    char title_buf[256];
    snprintf(title_buf, sizeof(title_buf), "%s %s",
             app_name ? app_name : "", version ? version : "");
    n = 0;
    IswSetArg(args[n], IswNlabel, title_buf);        n++;
    IswSetArg(args[n], IswNborderWidth, 0);          n++;
    if (prev) { IswSetArg(args[n], IswNfromVert, prev); n++; }
    prev = IswCreateManagedWidget("aboutTitle", labelWidgetClass,
                                  form, args, n);

    /* Description */
    if (description) {
        n = 0;
        IswSetArg(args[n], IswNlabel, description);     n++;
        IswSetArg(args[n], IswNborderWidth, 0);          n++;
        IswSetArg(args[n], IswNfromVert, prev);          n++;
        prev = IswCreateManagedWidget("aboutDesc", labelWidgetClass,
                                      form, args, n);
    }

    /* Close button */
    IsdeDialogButton btns[1] = {
        { "Close", about_close_cb, ctx },
    };
    isde_dialog_add_buttons(form, prev, 300 - 8 * 2,
                            btns, 1);

    isde_dialog_popup(ctx->shell, IswGrabExclusive);
    return ctx->shell;
}

/* ================================================================
 * Progress dialog
 * ================================================================ */

#define PROGRESS_SHOW_DELAY_MS 500

struct IsdeProgress {
    Widget       shell;
    Widget       bar;
    Widget       label;
    Widget       file_bar;
    Widget       file_label;
    IswIntervalId show_timer;
    IswAppContext app;
    Widget       parent;
    const char  *title;
    IswCallbackProc cancel_cb;
    void        *cancel_data;
    int          last_pct;
    int          last_file_pct;
    char         last_msg[128];
    char         last_file_msg[128];
};

static void progress_create_dialog(IsdeProgress *p)
{
    p->shell = isde_dialog_create_shell(p->parent, "progressShell",
                                        p->title, 350, 190);

    Arg args[20];
    Cardinal n = 0;
    IswSetArg(args[n], IswNorientation, XtorientVertical); n++;
    IswSetArg(args[n], IswNborderWidth, 0);                 n++;
    Widget vbox = IswCreateManagedWidget("progressBox", flexBoxWidgetClass,
                                         p->shell, args, n);

    /* Label — disable resize so text changes don't trigger relayout */
    n = 0;
    IswSetArg(args[n], IswNlabel, "");                       n++;
    IswSetArg(args[n], IswNborderWidth, 0);                  n++;
    IswSetArg(args[n], IswNjustify, IswJustifyLeft);          n++;
    IswSetArg(args[n], IswNresize, False);                   n++;
    p->label = IswCreateManagedWidget("progressLabel", labelWidgetClass,
                                      vbox, args, n);

    /* Progress bar */
    n = 0;
    IswSetArg(args[n], IswNvalue, 0);                        n++;
    IswSetArg(args[n], IswNborderWidth, 1);                  n++;
    IswSetArg(args[n], IswNflexGrow, 1);                     n++;
    p->bar = IswCreateManagedWidget("progressBar", progressBarWidgetClass,
                                    vbox, args, n);

    /* Per-file label — disable resize so text changes don't trigger relayout */
    n = 0;
    IswSetArg(args[n], IswNlabel, "");                       n++;
    IswSetArg(args[n], IswNborderWidth, 0);                  n++;
    IswSetArg(args[n], IswNjustify, IswJustifyLeft);          n++;
    IswSetArg(args[n], IswNresize, False);                   n++;
    p->file_label = IswCreateManagedWidget("fileLabel", labelWidgetClass,
                                           vbox, args, n);

    /* Per-file progress bar */
    n = 0;
    IswSetArg(args[n], IswNvalue, 0);                        n++;
    IswSetArg(args[n], IswNborderWidth, 1);                  n++;
    IswSetArg(args[n], IswNflexGrow, 1);                     n++;
    p->file_bar = IswCreateManagedWidget("fileBar", progressBarWidgetClass,
                                         vbox, args, n);

    /* Cancel button — right-aligned */
    n = 0;
    IswSetArg(args[n], IswNlabel, "Cancel");                 n++;
    IswSetArg(args[n], IswNwidth, 80);           n++;
    IswSetArg(args[n], IswNinternalWidth, 8);    n++;
    IswSetArg(args[n], IswNinternalHeight, 8);   n++;
    IswSetArg(args[n], IswNflexAlign, XtflexAlignEnd);       n++;
    Widget cancel = IswCreateManagedWidget("cancelBtn", commandWidgetClass,
                                           vbox, args, n);
    if (p->cancel_cb)
        IswAddCallback(cancel, IswNcallback, p->cancel_cb, p->cancel_data);

    isde_dialog_popup(p->shell, IswGrabNone);
}

static void progress_show_delay_cb(IswPointer closure, IswIntervalId *id)
{
    (void)id;
    IsdeProgress *p = (IsdeProgress *)closure;
    p->show_timer = 0;
    progress_create_dialog(p);
}

IsdeProgress *isde_progress_create(Widget parent, const char *title,
                                   IswAppContext app,
                                   IswCallbackProc cancel_cb, void *data)
{
    IsdeProgress *p = calloc(1, sizeof(*p));
    p->parent = parent;
    p->title = title;
    p->app = app;
    p->cancel_cb = cancel_cb;
    p->cancel_data = data;

    p->show_timer = IswAppAddTimeOut(app, PROGRESS_SHOW_DELAY_MS,
                                    progress_show_delay_cb, p);
    return p;
}

void isde_progress_update(IsdeProgress *p, int percent, const char *message)
{
    if (!p || !p->shell) return;

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    if (p->bar && percent != p->last_pct) {
        Arg a;
        IswSetArg(a, IswNvalue, percent);
        IswSetValues(p->bar, &a, 1);
        p->last_pct = percent;
    }
    if (p->label && message && strcmp(message, p->last_msg) != 0) {
        Arg a;
        IswSetArg(a, IswNlabel, message);
        IswSetValues(p->label, &a, 1);
        snprintf(p->last_msg, sizeof(p->last_msg), "%s", message);
    }
}

void isde_progress_update_file(IsdeProgress *p, int percent,
                               const char *message)
{
    if (!p || !p->shell) return;

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    if (p->file_bar && percent != p->last_file_pct) {
        Arg a;
        IswSetArg(a, IswNvalue, percent);
        IswSetValues(p->file_bar, &a, 1);
        p->last_file_pct = percent;
    }
    if (p->file_label && message &&
        strcmp(message, p->last_file_msg) != 0) {
        Arg a;
        IswSetArg(a, IswNlabel, message);
        IswSetValues(p->file_label, &a, 1);
        snprintf(p->last_file_msg, sizeof(p->last_file_msg), "%s", message);
    }
}

void isde_progress_destroy(IsdeProgress *p)
{
    if (!p) return;

    if (p->show_timer) {
        IswRemoveTimeOut(p->show_timer);
        p->show_timer = 0;
    }
    if (p->shell) {
        isde_dialog_dismiss(p->shell);
        p->shell = NULL;
        p->bar = NULL;
        p->label = NULL;
        p->file_bar = NULL;
        p->file_label = NULL;
    }
    free(p);
}
