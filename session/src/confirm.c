/*
 * confirm.c — standalone confirmation overlay (isde-confirm)
 *
 * Usage: isde-confirm <action>
 *   where <action> is: shutdown, reboot, suspend, logout
 *
 * Shows a fullscreen override-redirect overlay with a confirmation dialog.
 * Exits 0 if confirmed, 1 if cancelled.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <X11/keysym.h>

#include "isde/isde-theme.h"
#include "isde/isde-xdg.h"

#include <X11/Shell.h>
#include <X11/StringDefs.h>
#include <X11/IntrinsicP.h>
#include <ISW/Form.h>
#include <ISW/Label.h>
#include <ISW/Command.h>

static int result = 1;  /* default: cancelled */
static XtAppContext app;

static void action_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)cd; (void)call;
    result = 0;
    XtAppSetExitFlag(app);
}

static void cancel_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)cd; (void)call;
    result = 1;
    XtAppSetExitFlag(app);
}

static void key_handler(Widget w, XtPointer cd,
                        xcb_generic_event_t *xev, Boolean *cont)
{
    (void)w; (void)cd; (void)cont;
    if ((xev->response_type & ~0x80) != XCB_KEY_PRESS) {
        return;
    }
    xcb_key_press_event_t *kev = (xcb_key_press_event_t *)xev;
    xcb_connection_t *conn = XtDisplay(w);
    xcb_key_symbols_t *syms = xcb_key_symbols_alloc(conn);
    if (syms) {
        xcb_keysym_t sym = xcb_key_symbols_get_keysym(syms, kev->detail, 0);
        xcb_key_symbols_free(syms);
        if (sym == XK_Escape) {
            result = 1;
            XtAppSetExitFlag(app);
        } else if (sym == XK_Return || sym == XK_KP_Enter) {
            result = 0;
            XtAppSetExitFlag(app);
        }
    }
}

static void on_signal(int sig)
{
    (void)sig;
    result = 1;
    XtAppSetExitFlag(app);
}

int main(int argc, char **argv)
{
    /* Extract --action <name> before Xt sees it */
    const char *action = NULL;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--action") == 0) {
            action = argv[i + 1];
            int remaining = argc - i - 2;
            memmove(&argv[i], &argv[i + 2], remaining * sizeof(char *));
            argc -= 2;
            argv[argc] = NULL;
            break;
        }
    }

    if (!action) {
        fprintf(stderr, "Usage: isde-confirm --action <shutdown|reboot|suspend|logout>\n");
        return 1;
    }

    const char *message;
    const char *button_label;

    if (strcmp(action, "shutdown") == 0) {
        message = "Are you sure you want to shut down?";
        button_label = "Shut Down";
    } else if (strcmp(action, "reboot") == 0) {
        message = "Are you sure you want to reboot?";
        button_label = "Reboot";
    } else if (strcmp(action, "suspend") == 0) {
        message = "Are you sure you want to suspend?";
        button_label = "Suspend";
    } else if (strcmp(action, "logout") == 0) {
        message = "Are you sure you want to log out?";
        button_label = "Log Out";
    } else {
        fprintf(stderr, "isde-confirm: unknown action '%s'\n", action);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* Initialize Xt with theme resources */
    char **fallbacks = isde_theme_build_resources();
    Widget toplevel = XtAppInitialize(&app, "ISDE-Confirm",
                                      NULL, 0, &argc, argv,
                                      fallbacks, NULL, 0);

    /* Don't map the toplevel */
    Arg tl_args[20];
    Cardinal tl_n = 0;
    XtSetArg(tl_args[tl_n], XtNmappedWhenManaged, False); tl_n++;
    XtSetArg(tl_args[tl_n], XtNwidth, 1);                 tl_n++;
    XtSetArg(tl_args[tl_n], XtNheight, 1);                tl_n++;
    XtSetValues(toplevel, tl_args, tl_n);
    XtRealizeWidget(toplevel);

    xcb_screen_t *scr = XtScreen(toplevel);
    int scr_w = scr->width_in_pixels;
    int scr_h = scr->height_in_pixels;

    /* Colours */
    const IsdeColorScheme *scheme = isde_theme_current();
    xcb_connection_t *conn = XtDisplay(toplevel);
    Pixel overlay_bg = scr->black_pixel;
    Pixel form_bg = scr->white_pixel;
    Pixel form_fg = scr->black_pixel;

    if (scheme) {
        xcb_alloc_color_reply_t *r;
        r = xcb_alloc_color_reply(conn,
            xcb_alloc_color(conn, scr->default_colormap,
                            0x1000, 0x1000, 0x1000), NULL);
        if (r) { overlay_bg = r->pixel; free(r); }

        unsigned int bg = scheme->bg;
        r = xcb_alloc_color_reply(conn,
            xcb_alloc_color(conn, scr->default_colormap,
                            ((bg >> 16) & 0xFF) * 257,
                            ((bg >> 8)  & 0xFF) * 257,
                            ( bg        & 0xFF) * 257), NULL);
        if (r) { form_bg = r->pixel; free(r); }

        unsigned int fg = scheme->fg;
        r = xcb_alloc_color_reply(conn,
            xcb_alloc_color(conn, scr->default_colormap,
                            ((fg >> 16) & 0xFF) * 257,
                            ((fg >> 8)  & 0xFF) * 257,
                            ( fg        & 0xFF) * 257), NULL);
        if (r) { form_fg = r->pixel; free(r); }
    }

    /* Fullscreen override-redirect shell */
    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNx, 0);                      n++;
    XtSetArg(args[n], XtNy, 0);                      n++;
    XtSetArg(args[n], XtNwidth, scr_w);               n++;
    XtSetArg(args[n], XtNheight, scr_h);              n++;
    XtSetArg(args[n], XtNoverrideRedirect, True);     n++;
    XtSetArg(args[n], XtNborderWidth, 0);             n++;
    XtSetArg(args[n], XtNbackground, overlay_bg);     n++;
    Widget shell = XtCreatePopupShell("confirmOverlay",
                                      overrideShellWidgetClass,
                                      toplevel, args, n);

    /* Overlay form */
    n = 0;
    XtSetArg(args[n], XtNdefaultDistance, 0);         n++;
    XtSetArg(args[n], XtNborderWidth, 0);             n++;
    XtSetArg(args[n], XtNbackground, overlay_bg);     n++;
    Widget overlay = XtCreateManagedWidget("overlay", formWidgetClass,
                                           shell, args, n);

    /* Centered dialog form */
    int dlg_w = isde_scale(350);
    int dlg_h = isde_scale(150);
    int dlg_x = (scr_w - dlg_w) / 2;
    int dlg_y = (scr_h - dlg_h) / 2;

    n = 0;
    XtSetArg(args[n], XtNwidth, dlg_w);               n++;
    XtSetArg(args[n], XtNheight, dlg_h);              n++;
    XtSetArg(args[n], XtNborderWidth, 1);             n++;
    XtSetArg(args[n], XtNbackground, form_bg);        n++;
    XtSetArg(args[n], XtNdefaultDistance, isde_scale(8)); n++;
    Widget dialog = XtCreateManagedWidget("confirmDialog", formWidgetClass,
                                          overlay, args, n);

    /* Message label */
    n = 0;
    XtSetArg(args[n], XtNlabel, message);             n++;
    XtSetArg(args[n], XtNborderWidth, 0);             n++;
    XtSetArg(args[n], XtNbackground, form_bg);        n++;
    XtSetArg(args[n], XtNforeground, form_fg);        n++;
    XtSetArg(args[n], XtNtop, XtChainTop);            n++;
    XtSetArg(args[n], XtNbottom, XtChainTop);         n++;
    XtSetArg(args[n], XtNleft, XtChainLeft);          n++;
    XtSetArg(args[n], XtNright, XtChainRight);        n++;
    Widget label = XtCreateManagedWidget("confirmLabel", labelWidgetClass,
                                          dialog, args, n);

    /* Button row — affirmative first, Cancel last (per HIG) */
    int btn_w = isde_scale(80);
    int btn_pad = isde_scale(8);
    int total_btn = btn_w * 2 + btn_pad;
    int first_horiz = dlg_w - total_btn - btn_pad * 2;

    n = 0;
    XtSetArg(args[n], XtNlabel, button_label);        n++;
    XtSetArg(args[n], XtNwidth, btn_w);               n++;
    XtSetArg(args[n], XtNfromVert, label);             n++;
    XtSetArg(args[n], XtNhorizDistance, first_horiz);  n++;
    XtSetArg(args[n], XtNleft, XtChainRight);         n++;
    XtSetArg(args[n], XtNright, XtChainRight);        n++;
    XtSetArg(args[n], XtNtop, XtChainBottom);         n++;
    XtSetArg(args[n], XtNbottom, XtChainBottom);      n++;
    Widget action_btn = XtCreateManagedWidget("confirmBtn",
                                              commandWidgetClass,
                                              dialog, args, n);
    XtAddCallback(action_btn, XtNcallback, action_cb, NULL);

    n = 0;
    XtSetArg(args[n], XtNlabel, "Cancel");            n++;
    XtSetArg(args[n], XtNwidth, btn_w);               n++;
    XtSetArg(args[n], XtNfromVert, label);             n++;
    XtSetArg(args[n], XtNfromHoriz, action_btn);      n++;
    XtSetArg(args[n], XtNhorizDistance, btn_pad);      n++;
    XtSetArg(args[n], XtNleft, XtChainRight);         n++;
    XtSetArg(args[n], XtNright, XtChainRight);        n++;
    XtSetArg(args[n], XtNtop, XtChainBottom);         n++;
    XtSetArg(args[n], XtNbottom, XtChainBottom);      n++;
    Widget cancel_btn = XtCreateManagedWidget("cancelBtn",
                                              commandWidgetClass,
                                              dialog, args, n);
    XtAddCallback(cancel_btn, XtNcallback, cancel_cb, NULL);

    /* Escape / Enter key handling */
    XtAddEventHandler(shell, XCB_EVENT_MASK_KEY_PRESS, False,
                      key_handler, NULL);

    XtPopup(shell, XtGrabExclusive);

    /* Position dialog centered after realization */
    XtConfigureWidget(dialog, dlg_x, dlg_y, dlg_w, dlg_h, 1);

    /* Grab keyboard and pointer */
    xcb_window_t grab_win = XtWindow(shell);
    for (int attempt = 0; attempt < 50; attempt++) {
        xcb_grab_keyboard_reply_t *kr = xcb_grab_keyboard_reply(conn,
            xcb_grab_keyboard(conn, 1, grab_win,
                              XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC,
                              XCB_GRAB_MODE_ASYNC), NULL);
        int kb_ok = kr && kr->status == XCB_GRAB_STATUS_SUCCESS;
        free(kr);

        xcb_grab_pointer_reply_t *pr = xcb_grab_pointer_reply(conn,
            xcb_grab_pointer(conn, 1, grab_win,
                             XCB_EVENT_MASK_BUTTON_PRESS |
                             XCB_EVENT_MASK_BUTTON_RELEASE |
                             XCB_EVENT_MASK_POINTER_MOTION,
                             XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                             grab_win, XCB_NONE, XCB_CURRENT_TIME), NULL);
        int ptr_ok = pr && pr->status == XCB_GRAB_STATUS_SUCCESS;
        free(pr);

        if (kb_ok && ptr_ok) {
            break;
        }
        if (kb_ok) { xcb_ungrab_keyboard(conn, XCB_CURRENT_TIME); }
        if (ptr_ok) { xcb_ungrab_pointer(conn, XCB_CURRENT_TIME); }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 };
        nanosleep(&ts, NULL);
    }
    xcb_flush(conn);

    /* Event loop */
    while (!XtAppGetExitFlag(app)) {
        XtAppProcessEvent(app, XtIMAll);
    }

    xcb_ungrab_keyboard(conn, XCB_CURRENT_TIME);
    xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
    xcb_flush(conn);

    XtDestroyWidget(shell);
    XtDestroyWidget(toplevel);

    return result;
}
