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
#include <X11/StringDefs.h>
#include <X11/Shell.h>
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
    while (w && !XtIsShell(w))
        w = XtParent(w);
    isde_dialog_dismiss(w);
}

static XtActionsRec dialog_actions[] = {
    {"isde-dialog-dismiss", act_isde_dialog_dismiss},
};

static void ensure_actions_registered(Widget any_widget)
{
    static int registered;
    if (!registered) {
        XtAppAddActions(XtWidgetToApplicationContext(any_widget),
                        dialog_actions, XtNumber(dialog_actions));
        registered = 1;
    }
}

/* Find nearest Shell ancestor (or w itself if it is a shell) */
static Widget find_shell_ancestor(Widget w)
{
    while (w && !XtIsShell(w))
        w = XtParent(w);
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
    XtSetArg(args[n], XtNwidth, width);   n++;
    XtSetArg(args[n], XtNheight, height);  n++;
    XtSetArg(args[n], XtNborderWidth, 1);              n++;
    if (title) {
        XtSetArg(args[n], XtNtitle, title);            n++;
    }
    Widget shell = XtCreatePopupShell((String)name, transientShellWidgetClass,
                                      shell_parent, args, n);

    /* Escape and WM close button both dismiss */
    XtOverrideTranslations(shell, XtParseTranslationTable(
        "<Message>WM_PROTOCOLS: isde-dialog-dismiss()\n"
        "<Key>Escape: isde-dialog-dismiss()\n"));

    return shell;
}

void isde_dialog_popup(Widget shell, XtGrabKind grab)
{
    if (shell)
        XtPopup(shell, grab);
}

void isde_dialog_dismiss(Widget shell)
{
    if (!shell)
        return;
    XtPopdown(shell);
    XtDestroyWidget(shell);
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
        XtSetArg(args[n], XtNlabel, buttons[i].label);      n++;
        XtSetArg(args[n], XtNwidth, btn_w);                  n++;
        XtSetArg(args[n], XtNinternalWidth, btn_pad);        n++;
        XtSetArg(args[n], XtNinternalHeight, btn_pad);       n++;
        if (above_widget) {
            XtSetArg(args[n], XtNfromVert, above_widget);    n++;
        }
        if (i == 0) {
            XtSetArg(args[n], XtNhorizDistance, first_horiz); n++;
        } else {
            XtSetArg(args[n], XtNfromHoriz, prev);           n++;
            XtSetArg(args[n], XtNhorizDistance, btn_pad);    n++;
        }
        XtSetArg(args[n], XtNleft, XtChainRight);            n++;
        XtSetArg(args[n], XtNright, XtChainRight);           n++;
        XtSetArg(args[n], XtNbottom, XtChainBottom);         n++;
        XtSetArg(args[n], XtNtop, XtChainBottom);            n++;
        Widget btn = XtCreateManagedWidget("btn", commandWidgetClass,
                                           form, args, n);
        XtAddCallback(btn, XtNcallback, buttons[i].callback,
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

static void confirm_action_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    DialogCtx *ctx = (DialogCtx *)cd;
    if (ctx->result_cb)
        ctx->result_cb(ISDE_DIALOG_OK, ctx->user_data);
    ctx_dismiss_and_free(ctx);
}

static void confirm_cancel_cb(Widget w, XtPointer cd, XtPointer call)
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
    XtSetArg(args[n], XtNlabel, message); n++;
    Widget dialog = XtCreateManagedWidget("confirmDialog", dialogWidgetClass,
                                          ctx->shell, args, n);

    Widget anchor = XtNameToWidget(dialog, "label");

    IsdeDialogButton btns[2] = {
        { action_label, confirm_action_cb, ctx },
        { "Cancel",     confirm_cancel_cb, ctx },
    };
    isde_dialog_add_buttons(dialog, anchor, 300, btns, 2);

    isde_dialog_popup(ctx->shell, XtGrabExclusive);
    return ctx->shell;
}

/* ================================================================
 * Message dialog
 * ================================================================ */

static void message_ok_cb(Widget w, XtPointer cd, XtPointer call)
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
    XtSetArg(args[n], XtNlabel, message); n++;
    Widget dialog = XtCreateManagedWidget("messageDialog", dialogWidgetClass,
                                          ctx->shell, args, n);

    Widget anchor = XtNameToWidget(dialog, "label");

    IsdeDialogButton btns[1] = {
        { "OK", message_ok_cb, ctx },
    };
    isde_dialog_add_buttons(dialog, anchor, 300, btns, 1);

    isde_dialog_popup(ctx->shell, XtGrabExclusive);
    return ctx->shell;
}

/* ================================================================
 * Input dialog
 * ================================================================ */

static void input_ok_cb(Widget w, XtPointer cd, XtPointer call)
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

static void input_cancel_cb(Widget w, XtPointer cd, XtPointer call)
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
    XtSetArg(args[n], XtNlabel, prompt);                    n++;
    if (initial_value) {
        XtSetArg(args[n], XtNvalue, initial_value);         n++;
    }
    ctx->dialog_widget = XtCreateManagedWidget("inputDialog",
                                               dialogWidgetClass,
                                               ctx->shell, args, n);

    Widget value_w = XtNameToWidget(ctx->dialog_widget, "value");
    Widget anchor = value_w ? value_w :
                    XtNameToWidget(ctx->dialog_widget, "label");

    IsdeDialogButton btns[2] = {
        { "OK",     input_ok_cb,     ctx },
        { "Cancel", input_cancel_cb, ctx },
    };
    isde_dialog_add_buttons(ctx->dialog_widget, anchor,
                            300, btns, 2);

    isde_dialog_popup(ctx->shell, XtGrabExclusive);
    return ctx->shell;
}

/* ================================================================
 * Font chooser dialog
 * ================================================================ */

static void font_ok_cb(Widget w, XtPointer cd, XtPointer call)
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

static void font_cancel_cb(Widget w, XtPointer cd, XtPointer call)
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
    XtSetArg(args[n], XtNdefaultDistance, 8); n++;
    XtSetArg(args[n], XtNborderWidth, 0);                 n++;
    Widget form = XtCreateManagedWidget("fcForm", formWidgetClass,
                                        ctx->shell, args, n);

    /* FontChooser widget */
    n = 0;
    if (initial_family) {
        XtSetArg(args[n], XtNfontFamily, initial_family); n++;
    }
    XtSetArg(args[n], XtNfontSize, initial_size);          n++;
    XtSetArg(args[n], XtNborderWidth, 0);                  n++;
    XtSetArg(args[n], XtNtop, XtChainTop);                 n++;
    XtSetArg(args[n], XtNbottom, XtChainBottom);           n++;
    XtSetArg(args[n], XtNleft, XtChainLeft);               n++;
    XtSetArg(args[n], XtNright, XtChainRight);             n++;
    XtSetArg(args[n], XtNwidth, 390);          n++;
    XtSetArg(args[n], XtNheight, 290);         n++;
    ctx->chooser_widget = XtCreateManagedWidget("fontChooser",
                                                 fontChooserWidgetClass,
                                                 form, args, n);

    IsdeDialogButton btns[2] = {
        { "OK",     font_ok_cb,     ctx },
        { "Cancel", font_cancel_cb, ctx },
    };
    isde_dialog_add_buttons(form, ctx->chooser_widget,
                            400 - 8 * 2, btns, 2);

    isde_dialog_popup(ctx->shell, XtGrabExclusive);
    return ctx->shell;
}

/* ================================================================
 * About dialog
 * ================================================================ */

static void about_close_cb(Widget w, XtPointer cd, XtPointer call)
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
    XtSetArg(args[n], XtNdefaultDistance, 8); n++;
    XtSetArg(args[n], XtNborderWidth, 0);                 n++;
    Widget form = XtCreateManagedWidget("aboutForm", formWidgetClass,
                                        ctx->shell, args, n);

    Widget prev = NULL;

    /* Icon (optional) */
    if (icon_path) {
        n = 0;
        XtSetArg(args[n], XtNimage, icon_path);        n++;
        XtSetArg(args[n], XtNlabel, "");               n++;
        XtSetArg(args[n], XtNborderWidth, 0);          n++;
        prev = XtCreateManagedWidget("aboutIcon", labelWidgetClass,
                                      form, args, n);
    }

    /* App name + version */
    char title_buf[256];
    snprintf(title_buf, sizeof(title_buf), "%s %s",
             app_name ? app_name : "", version ? version : "");
    n = 0;
    XtSetArg(args[n], XtNlabel, title_buf);        n++;
    XtSetArg(args[n], XtNborderWidth, 0);          n++;
    if (prev) { XtSetArg(args[n], XtNfromVert, prev); n++; }
    prev = XtCreateManagedWidget("aboutTitle", labelWidgetClass,
                                  form, args, n);

    /* Description */
    if (description) {
        n = 0;
        XtSetArg(args[n], XtNlabel, description);     n++;
        XtSetArg(args[n], XtNborderWidth, 0);          n++;
        XtSetArg(args[n], XtNfromVert, prev);          n++;
        prev = XtCreateManagedWidget("aboutDesc", labelWidgetClass,
                                      form, args, n);
    }

    /* Close button */
    IsdeDialogButton btns[1] = {
        { "Close", about_close_cb, ctx },
    };
    isde_dialog_add_buttons(form, prev, 300 - 8 * 2,
                            btns, 1);

    isde_dialog_popup(ctx->shell, XtGrabExclusive);
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
    XtIntervalId show_timer;
    XtAppContext app;
    Widget       parent;
    const char  *title;
    XtCallbackProc cancel_cb;
    void        *cancel_data;
};

static void progress_create_dialog(IsdeProgress *p)
{
    p->shell = isde_dialog_create_shell(p->parent, "progressShell",
                                        p->title, 350, 120);

    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNorientation, XtorientVertical); n++;
    XtSetArg(args[n], XtNborderWidth, 0);                 n++;
    Widget vbox = XtCreateManagedWidget("progressBox", flexBoxWidgetClass,
                                         p->shell, args, n);

    /* Label */
    n = 0;
    XtSetArg(args[n], XtNlabel, "");                       n++;
    XtSetArg(args[n], XtNborderWidth, 0);                  n++;
    XtSetArg(args[n], XtNjustify, XtJustifyLeft);          n++;
    p->label = XtCreateManagedWidget("progressLabel", labelWidgetClass,
                                      vbox, args, n);

    /* Progress bar */
    n = 0;
    XtSetArg(args[n], XtNvalue, 0);                        n++;
    XtSetArg(args[n], XtNborderWidth, 1);                  n++;
    XtSetArg(args[n], XtNflexGrow, 1);                     n++;
    p->bar = XtCreateManagedWidget("progressBar", progressBarWidgetClass,
                                    vbox, args, n);

    /* Cancel button — right-aligned */
    n = 0;
    XtSetArg(args[n], XtNlabel, "Cancel");                 n++;
    XtSetArg(args[n], XtNwidth, 80);           n++;
    XtSetArg(args[n], XtNinternalWidth, 8);    n++;
    XtSetArg(args[n], XtNinternalHeight, 8);   n++;
    XtSetArg(args[n], XtNflexAlign, XtflexAlignEnd);       n++;
    Widget cancel = XtCreateManagedWidget("cancelBtn", commandWidgetClass,
                                           vbox, args, n);
    if (p->cancel_cb)
        XtAddCallback(cancel, XtNcallback, p->cancel_cb, p->cancel_data);

    isde_dialog_popup(p->shell, XtGrabNone);
}

static void progress_show_delay_cb(XtPointer closure, XtIntervalId *id)
{
    (void)id;
    IsdeProgress *p = (IsdeProgress *)closure;
    p->show_timer = 0;
    progress_create_dialog(p);
}

IsdeProgress *isde_progress_create(Widget parent, const char *title,
                                   XtAppContext app,
                                   XtCallbackProc cancel_cb, void *data)
{
    IsdeProgress *p = calloc(1, sizeof(*p));
    p->parent = parent;
    p->title = title;
    p->app = app;
    p->cancel_cb = cancel_cb;
    p->cancel_data = data;

    p->show_timer = XtAppAddTimeOut(app, PROGRESS_SHOW_DELAY_MS,
                                    progress_show_delay_cb, p);
    return p;
}

void isde_progress_update(IsdeProgress *p, int percent, const char *message)
{
    if (!p || !p->shell) return;

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    if (p->bar) {
        Arg a;
        XtSetArg(a, XtNvalue, percent);
        XtSetValues(p->bar, &a, 1);
    }
    if (p->label && message) {
        Arg a;
        XtSetArg(a, XtNlabel, message);
        XtSetValues(p->label, &a, 1);
    }
}

void isde_progress_destroy(IsdeProgress *p)
{
    if (!p) return;

    if (p->show_timer) {
        XtRemoveTimeOut(p->show_timer);
        p->show_timer = 0;
    }
    if (p->shell) {
        isde_dialog_dismiss(p->shell);
        p->shell = NULL;
        p->bar = NULL;
        p->label = NULL;
    }
    free(p);
}
