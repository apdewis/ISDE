#define _POSIX_C_SOURCE 200809L
/*
 * isde-dialog.c — HIG-compliant dialog helpers and standard dialogs
 */
#include "isde-dialog.h"
#include "ewmh.h"
#include "isde-xdg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ISW/StringDefs.h>
#include <ISW/Shell.h>
#include <ISW/Command.h>
#include <ISW/Label.h>
#include <ISW/Form.h>
#include <ISW/FlexBox.h>
#include <ISW/Dialog.h>
#include <ISW/Text.h>
#include <ISW/FontChooser.h>
#include <fontconfig/fontconfig.h>
#include <ISW/ProgressBar.h>
#include <ISW/IswArgMacros.h>
#include <ISW/IntrinsicP.h>
#include <ISW/ISWRender.h>
#include <ISW/ISWPlatform.h>
#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>

/* ================================================================
 * Internal: Xt action for dismiss (registered once)
 * ================================================================ */

static void act_isde_dialog_dismiss(Widget w, IswEvent *ev,
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

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgWidth(&ab, width);
    IswArgHeight(&ab, height);
    IswArgBorderWidth(&ab, 1);
    if (title) {
        IswArgTitle(&ab, title);
    }
    Widget shell = IswCreatePopupShell((String)name, transientShellWidgetClass,
                                      shell_parent, ab.args, ab.count);

    /* Escape and WM close button both dismiss */
    IswOverrideTranslations(shell, IswParseTranslationTable(
        "<Message>WM_PROTOCOLS: isde-dialog-dismiss()\n"
        "<Key>Escape: isde-dialog-dismiss()\n"));

    return shell;
}

void isde_dialog_popup(Widget shell, IswGrabKind grab)
{
    if (!shell) {
        return;
    }

    /* Set _NET_WM_STATE_ABOVE before mapping so the WM sees it */
    IswRealizeWidget(shell);
    xcb_connection_t *conn =
        (xcb_connection_t *)IswDisplayNativeHandle(IswDisplayOf(shell));
    IsdeEwmh *ewmh = isde_ewmh_init(conn, 0);
    if (ewmh) {
        xcb_ewmh_connection_t *ec = isde_ewmh_connection(ewmh);
        xcb_atom_t above = ec->_NET_WM_STATE_ABOVE;
        xcb_window_t win = (xcb_window_t)(uintptr_t)IswWindowNativeHandle(
            _IswPlatformWidgetWindow(IswDisplayOf(shell), shell));
        xcb_ewmh_set_wm_state(ec, win, 1, &above);
        xcb_flush(conn);
    }

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
        IswArgBuilder ab = IswArgBuilderInit();
        IswArgLabel(&ab, buttons[i].label);
        IswArgWidth(&ab, btn_w);
        IswArgInternalWidth(&ab, btn_pad);
        IswArgInternalHeight(&ab, btn_pad);
        if (above_widget) {
            IswArgFromVert(&ab, above_widget);
        }
        if (i == 0) {
            IswArgHorizDistance(&ab, first_horiz);
        } else {
            IswArgFromHoriz(&ab, prev);
            IswArgHorizDistance(&ab, btn_pad);
        }
        IswArgLeft(&ab, IswChainRight);
        IswArgRight(&ab, IswChainRight);
        IswArgBottom(&ab, IswChainBottom);
        IswArgTop(&ab, IswChainBottom);
        Widget btn = IswCreateManagedWidget("btn", commandWidgetClass,
                                           form, ab.args, ab.count);
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

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgLabel(&ab, message);
    Widget dialog = IswCreateManagedWidget("confirmDialog", dialogWidgetClass,
                                          ctx->shell, ab.args, ab.count);

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

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgLabel(&ab, message);
    Widget dialog = IswCreateManagedWidget("messageDialog", dialogWidgetClass,
                                          ctx->shell, ab.args, ab.count);

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

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgLabel(&ab, prompt);
    if (initial_value) {
        IswArgValue(&ab, initial_value);
    }
    ctx->dialog_widget = IswCreateManagedWidget("inputDialog",
                                               dialogWidgetClass,
                                               ctx->shell, ab.args, ab.count);

    Widget value_w = IswNameToWidget(ctx->dialog_widget, "value");
    if (value_w) {
        IswArgBuilder vab = IswArgBuilderInit();
        IswArgResize(&vab, IswtextResizeNever);
        IswSetValues(value_w, vab.args, vab.count);
    }
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
    int weight = FC_WEIGHT_NORMAL;
    int slant = FC_SLANT_ROMAN;
    if (ctx->chooser_widget) {
        family = IswFontChooserGetFamily(ctx->chooser_widget);
        size = IswFontChooserGetSize(ctx->chooser_widget);
        weight = IswFontChooserGetWeight(ctx->chooser_widget);
        slant = IswFontChooserGetSlant(ctx->chooser_widget);
    }
    if (ctx->font_cb)
        ctx->font_cb(ISDE_DIALOG_OK, family, size, weight, slant,
                     ctx->user_data);
    ctx_dismiss_and_free(ctx);
}

static void font_cancel_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    DialogCtx *ctx = (DialogCtx *)cd;
    if (ctx->font_cb)
        ctx->font_cb(ISDE_DIALOG_CANCEL, NULL, 0, 0, 0, ctx->user_data);
    ctx_dismiss_and_free(ctx);
}

Widget isde_dialog_font(Widget parent, const char *title,
                        const char *initial_family, int initial_size,
                        int initial_weight, int initial_slant,
                        IsdeDialogFontCB callback, void *data)
{
    DialogCtx *ctx = ctx_new();
    ctx->font_cb = callback;
    ctx->user_data = data;

    ctx->shell = isde_dialog_create_shell(parent, "fontChooserShell",
                                          title, 400, 350);

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgDefaultDistance(&ab, 8);
    IswArgBorderWidth(&ab, 0);
    Widget form = IswCreateManagedWidget("fcForm", formWidgetClass,
                                        ctx->shell, ab.args, ab.count);

    /* FontChooser widget */
    IswArgBuilderReset(&ab);
    if (initial_family) {
        IswArgFontFamily(&ab, initial_family);
    }
    IswArgFontSize(&ab, initial_size);
    IswArgFontWeight(&ab, initial_weight);
    IswArgFontSlant(&ab, initial_slant);
    IswArgBorderWidth(&ab, 0);
    IswArgTop(&ab, IswChainTop);
    IswArgBottom(&ab, IswChainBottom);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainRight);
    IswArgWidth(&ab, 390);
    IswArgHeight(&ab, 290);
    ctx->chooser_widget = IswCreateManagedWidget("fontChooser",
                                                 fontChooserWidgetClass,
                                                 form, ab.args, ab.count);

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

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgDefaultDistance(&ab, 8);
    IswArgBorderWidth(&ab, 0);
    Widget form = IswCreateManagedWidget("aboutForm", formWidgetClass,
                                        ctx->shell, ab.args, ab.count);

    Widget prev = NULL;

    /* Icon (optional) */
    if (icon_path) {
        IswArgBuilderReset(&ab);
        IswArgImage(&ab, icon_path);
        IswArgLabel(&ab, "");
        IswArgBorderWidth(&ab, 0);
        prev = IswCreateManagedWidget("aboutIcon", labelWidgetClass,
                                      form, ab.args, ab.count);
    }

    /* App name + version */
    char title_buf[256];
    snprintf(title_buf, sizeof(title_buf), "%s %s",
             app_name ? app_name : "", version ? version : "");
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, title_buf);
    IswArgBorderWidth(&ab, 0);
    if (prev) { IswArgFromVert(&ab, prev); }
    prev = IswCreateManagedWidget("aboutTitle", labelWidgetClass,
                                  form, ab.args, ab.count);

    /* Description */
    if (description) {
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, description);
        IswArgBorderWidth(&ab, 0);
        IswArgFromVert(&ab, prev);
        prev = IswCreateManagedWidget("aboutDesc", labelWidgetClass,
                                      form, ab.args, ab.count);
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

