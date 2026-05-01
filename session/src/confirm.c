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
#include <xcb/randr.h>
#include <X11/keysym.h>

#include "isde/isde-theme.h"
#include "isde/isde-xdg.h"

#include <ISW/Shell.h>
#include <ISW/StringDefs.h>
#include <ISW/IntrinsicP.h>
#include <ISW/Form.h>
#include <ISW/Label.h>
#include <ISW/Command.h>
#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>

static void get_primary_geometry(xcb_connection_t *conn, xcb_window_t root,
                                 xcb_screen_t *scr,
                                 int *ox, int *oy, int *ow, int *oh)
{
    *ox = 0; *oy = 0;
    *ow = scr->width_in_pixels;
    *oh = scr->height_in_pixels;

    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(conn,
            xcb_randr_get_screen_resources_current(conn, root), NULL);
    if (!res) return;

    xcb_timestamp_t ts = res->config_timestamp;
    xcb_randr_get_output_primary_reply_t *pri =
        xcb_randr_get_output_primary_reply(conn,
            xcb_randr_get_output_primary(conn, root), NULL);
    xcb_randr_output_t primary_id = pri ? pri->output : XCB_NONE;
    free(pri);

    xcb_randr_output_t *outs =
        xcb_randr_get_screen_resources_current_outputs(res);
    int nouts = xcb_randr_get_screen_resources_current_outputs_length(res);

    xcb_randr_crtc_t fallback_crtc = XCB_NONE;

    for (int i = 0; i < nouts; i++) {
        xcb_randr_get_output_info_reply_t *oi =
            xcb_randr_get_output_info_reply(conn,
                xcb_randr_get_output_info(conn, outs[i], ts), NULL);
        if (!oi) continue;
        if (oi->connection != XCB_RANDR_CONNECTION_CONNECTED ||
            oi->crtc == XCB_NONE) {
            free(oi);
            continue;
        }
        if (fallback_crtc == XCB_NONE)
            fallback_crtc = oi->crtc;
        if (outs[i] == primary_id) {
            xcb_randr_get_crtc_info_reply_t *ci =
                xcb_randr_get_crtc_info_reply(conn,
                    xcb_randr_get_crtc_info(conn, oi->crtc, ts), NULL);
            if (ci) {
                *ox = ci->x; *oy = ci->y;
                *ow = ci->width; *oh = ci->height;
                free(ci);
            }
            free(oi);
            free(res);
            return;
        }
        free(oi);
    }

    if (fallback_crtc != XCB_NONE) {
        xcb_randr_get_crtc_info_reply_t *ci =
            xcb_randr_get_crtc_info_reply(conn,
                xcb_randr_get_crtc_info(conn, fallback_crtc, ts), NULL);
        if (ci) {
            *ox = ci->x; *oy = ci->y;
            *ow = ci->width; *oh = ci->height;
            free(ci);
        }
    }

    free(res);
}

static int result = 1;  /* default: cancelled */
static IswAppContext app;

static void action_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)cd; (void)call;
    result = 0;
    IswAppSetExitFlag(app);
}

static void cancel_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)cd; (void)call;
    result = 1;
    IswAppSetExitFlag(app);
}

static void key_handler(Widget w, IswPointer cd,
                        xcb_generic_event_t *xev, Boolean *cont)
{
    (void)w; (void)cd; (void)cont;
    if ((xev->response_type & ~0x80) != XCB_KEY_PRESS) {
        return;
    }
    xcb_key_press_event_t *kev = (xcb_key_press_event_t *)xev;
    xcb_connection_t *conn = IswDisplay(w);
    xcb_key_symbols_t *syms = xcb_key_symbols_alloc(conn);
    if (syms) {
        xcb_keysym_t sym = xcb_key_symbols_get_keysym(syms, kev->detail, 0);
        xcb_key_symbols_free(syms);
        if (sym == XK_Escape) {
            result = 1;
            IswAppSetExitFlag(app);
        } else if (sym == XK_Return || sym == XK_KP_Enter) {
            result = 0;
            IswAppSetExitFlag(app);
        }
    }
}

static void on_signal(int sig)
{
    (void)sig;
    result = 1;
    IswAppSetExitFlag(app);
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

    Widget toplevel = IswAppInitialize(&app, "ISDE-Confirm",
                                      NULL, 0, &argc, argv,
                                      NULL, NULL, 0);
    isde_theme_merge_xrm(toplevel);

    /* Don't map the toplevel */
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgMappedWhenManaged(&ab, False);
    IswArgWidth(&ab, 1);
    IswArgHeight(&ab, 1);
    IswSetValues(toplevel, ab.args, ab.count);
    IswRealizeWidget(toplevel);

    xcb_screen_t *scr = IswScreen(toplevel);
    xcb_connection_t *xconn = IswDisplay(toplevel);
    double sf = ISWScaleFactor(toplevel);
    if (sf < 1.0) { sf = 1.0; }

    int prim_x, prim_y, prim_w, prim_h;
    get_primary_geometry(xconn, scr->root, scr, &prim_x, &prim_y,
                         &prim_w, &prim_h);
    int scr_x = (int)(prim_x / sf + 0.5);
    int scr_y = (int)(prim_y / sf + 0.5);
    int scr_w = (int)(prim_w / sf + 0.5);
    int scr_h = (int)(prim_h / sf + 0.5);

    /* Colours */
    const IsdeColorScheme *scheme = isde_theme_current();
    xcb_connection_t *conn = IswDisplay(toplevel);
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

    /* Override-redirect shell covering primary monitor */
    IswArgBuilderReset(&ab);
    IswArgX(&ab, scr_x);
    IswArgY(&ab, scr_y);
    IswArgWidth(&ab, scr_w);
    IswArgHeight(&ab, scr_h);
    IswArgOverrideRedirect(&ab, True);
    IswArgBorderWidth(&ab, 0);
    IswArgBackground(&ab, overlay_bg);
    Widget shell = IswCreatePopupShell("confirmOverlay",
                                      overrideShellWidgetClass,
                                      toplevel, ab.args, ab.count);

    /* Overlay form */
    IswArgBuilderReset(&ab);
    IswArgDefaultDistance(&ab, 0);
    IswArgBorderWidth(&ab, 0);
    IswArgBackground(&ab, overlay_bg);
    Widget overlay = IswCreateManagedWidget("overlay", formWidgetClass,
                                           shell, ab.args, ab.count);

    /* Centered dialog form */
    int dlg_w = 350;
    int dlg_h = 150;

    IswArgBuilderReset(&ab);
    IswArgWidth(&ab, dlg_w);
    IswArgHeight(&ab, dlg_h);
    IswArgBorderWidth(&ab, 1);
    IswArgBackground(&ab, form_bg);
    IswArgDefaultDistance(&ab, 8);
    Widget dialog = IswCreateManagedWidget("confirmDialog", formWidgetClass,
                                          overlay, ab.args, ab.count);

    /* Message label */
    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, message);
    IswArgBorderWidth(&ab, 0);
    IswArgBackground(&ab, form_bg);
    IswArgForeground(&ab, form_fg);
    IswArgTop(&ab, IswChainTop);
    IswArgBottom(&ab, IswChainTop);
    IswArgLeft(&ab, IswChainLeft);
    IswArgRight(&ab, IswChainRight);
    Widget label = IswCreateManagedWidget("confirmLabel", labelWidgetClass,
                                          dialog, ab.args, ab.count);

    /* Button row — affirmative first, Cancel last (per HIG) */
    int btn_w = 80;
    int btn_pad = 8;
    int total_btn = btn_w * 2 + btn_pad;
    int first_horiz = dlg_w - total_btn - btn_pad * 2;

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, button_label);
    IswArgWidth(&ab, btn_w);
    IswArgFromVert(&ab, label);
    IswArgHorizDistance(&ab, first_horiz);
    IswArgLeft(&ab, IswChainRight);
    IswArgRight(&ab, IswChainRight);
    IswArgTop(&ab, IswChainBottom);
    IswArgBottom(&ab, IswChainBottom);
    Widget action_btn = IswCreateManagedWidget("confirmBtn",
                                              commandWidgetClass,
                                              dialog, ab.args, ab.count);
    IswAddCallback(action_btn, IswNcallback, action_cb, NULL);

    IswArgBuilderReset(&ab);
    IswArgLabel(&ab, "Cancel");
    IswArgWidth(&ab, btn_w);
    IswArgFromVert(&ab, label);
    IswArgFromHoriz(&ab, action_btn);
    IswArgHorizDistance(&ab, btn_pad);
    IswArgLeft(&ab, IswChainRight);
    IswArgRight(&ab, IswChainRight);
    IswArgTop(&ab, IswChainBottom);
    IswArgBottom(&ab, IswChainBottom);
    Widget cancel_btn = IswCreateManagedWidget("cancelBtn",
                                              commandWidgetClass,
                                              dialog, ab.args, ab.count);
    IswAddCallback(cancel_btn, IswNcallback, cancel_cb, NULL);

    /* Escape / Enter key handling */
    IswAddEventHandler(shell, XCB_EVENT_MASK_KEY_PRESS, False,
                      key_handler, NULL);

    IswPopup(shell, IswGrabExclusive);

    /* Center dialog — all values logical; ISW scales to physical internally */
    int cx = (scr_w - dlg_w) / 2;
    int cy = (scr_h - dlg_h) / 2;
    IswConfigureWidget(dialog, cx, cy, dlg_w, dlg_h, 1);

    /* Grab keyboard and pointer */
    xcb_window_t grab_win = IswWindow(shell);
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
    while (!IswAppGetExitFlag(app)) {
        IswAppProcessEvent(app, IswIMAll);
    }

    xcb_ungrab_keyboard(conn, XCB_CURRENT_TIME);
    xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
    xcb_flush(conn);

    IswDestroyWidget(shell);
    IswDestroyWidget(toplevel);

    return result;
}
