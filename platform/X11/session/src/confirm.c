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
#include <xcb/xcb_aux.h>

#include "isde-theme.h"
#include "isde-monitor-xcb.h"
#include "randr.h"
#include "isde-xdg.h"

#include <ISW/Shell.h>
#include <ISW/StringDefs.h>
#include <ISW/IntrinsicP.h>
#include <ISW/Form.h>
#include <ISW/Label.h>
#include <ISW/Command.h>
#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>

static int create_blank_screens(const IsdeMonitorOps *ops,
                                 IsdeMonitorXcbCtx *mon_ctx,
                                 xcb_connection_t *conn, xcb_screen_t *scr,
                                 IsdeMonitor *primary, uint32_t bg_pixel,
                                 xcb_window_t **out_wins)
{
    *out_wins = NULL;

    IsdeMonitor *mons = NULL;
    int nmons = ops ? ops->get_monitors(mon_ctx, &mons) : 0;
    if (nmons <= 0) { free(mons); return 0; }

    *out_wins = malloc(nmons * sizeof(xcb_window_t));
    int count = 0;

    for (int i = 0; i < nmons; i++) {
        if (mons[i].x == primary->x && mons[i].y == primary->y &&
            mons[i].width == primary->width &&
            mons[i].height == primary->height)
            continue;

        xcb_window_t win = xcb_generate_id(conn);
        uint32_t vals[] = { bg_pixel, 1, XCB_EVENT_MASK_NO_EVENT };
        xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, scr->root,
                          mons[i].x, mons[i].y, mons[i].width, mons[i].height,
                          0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          scr->root_visual,
                          XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
                          XCB_CW_EVENT_MASK, vals);
        xcb_map_window(conn, win);
        (*out_wins)[count++] = win;
    }

    xcb_flush(conn);
    free(mons);
    return count;
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
                        IswEvent *ev, Boolean *cont)
{
    (void)w; (void)cd; (void)cont;
    if (ev->kind != IswKeyDown) {
        return;
    }
    if (ev->key.key == IswKeyEscape) {
        result = 1;
        IswAppSetExitFlag(app);
    } else if (ev->key.key == IswKeyReturn) {
        result = 0;
        IswAppSetExitFlag(app);
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

    /* Own X connection for the raw xcb work (RandR query, blank windows,
     * grabs).  confirm is a separate process from the session core; it opens
     * its own connection rather than reaching into ISW for the native one. */
    int conn_screen;
    xcb_connection_t *conn = xcb_connect(getenv("DISPLAY"), &conn_screen);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "isde-confirm: cannot connect to X\n");
        return 1;
    }
    xcb_screen_t *scr = xcb_aux_get_screen(conn, conn_screen);

    double sf = ISWScaleFactor(toplevel);
    if (sf < 1.0) { sf = 1.0; }

    const IsdeMonitorOps *mon_ops = isde_monitor_xcb_probe(conn);
    IsdeMonitorXcbCtx mon_ctx = { conn, scr->root, scr };

    IsdeMonitor primary = { 0, 0, scr->width_in_pixels, scr->height_in_pixels };
    if (mon_ops) {
        mon_ops->get_primary(&mon_ctx, &primary);
    }
    int scr_x = (int)(primary.x / sf + 0.5);
    int scr_y = (int)(primary.y / sf + 0.5);
    int scr_w = (int)(primary.width / sf + 0.5);
    int scr_h = (int)(primary.height / sf + 0.5);

    /* Colours */
    const IsdeColorScheme *scheme = isde_theme_current();
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
    IswAddEventHandler(shell, IswKeyPressMask, False,
                      key_handler, NULL);

    xcb_window_t *blank_wins = NULL;
    int nblanks = create_blank_screens(mon_ops, &mon_ctx, conn, scr,
                                       &primary, overlay_bg, &blank_wins);

    IswPopup(shell, IswGrabExclusive);

    /* Center dialog after popup so the shell's layout pass doesn't override it */
    int cx = (scr_w - dlg_w) / 2;
    int cy = (scr_h - dlg_h) / 2;
    IswConfigureWidget(dialog, cx, cy, dlg_w, dlg_h, 1);

    /* Active X server grab so the override-redirect overlay receives all
     * input.  Must use the same connection ISW uses (via the separate conn
     * the grab steals events from ISW's dispatch).  Grab the shell widget
     * directly through IswGrabKeyboard/IswGrabPointer. */
    for (int attempt = 0; attempt < 50; attempt++) {
        int kb = IswGrabKeyboard(shell, True, 1 /* async */,
                                 1 /* async */, 0);
        int ptr = IswGrabPointer(shell, True,
                                  IswButtonPressMask | IswButtonReleaseMask |
                                  IswPointerMotionMask,
                                  1 /* async */, 1 /* async */,
                                  (IswWindow)None, (IswCursor)None, 0);
        if (kb == IswGrabSuccess && ptr == IswGrabSuccess)
            break;
        if (kb == IswGrabSuccess) IswUngrabKeyboard(shell, 0);
        if (ptr == IswGrabSuccess) IswUngrabPointer(shell, 0);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 };
        nanosleep(&ts, NULL);
    }

    /* Event loop */
    while (!IswAppGetExitFlag(app)) {
        IswAppProcessEvent(app, IswIMAll);
    }

    IswUngrabKeyboard(shell, 0);
    IswUngrabPointer(shell, 0);

    for (int i = 0; i < nblanks; i++)
        xcb_destroy_window(conn, blank_wins[i]);
    free(blank_wins);

    IswDestroyWidget(shell);
    IswDestroyWidget(toplevel);

    return result;
}
