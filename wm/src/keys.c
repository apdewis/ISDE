#define _POSIX_C_SOURCE 200809L
/*
 * keys.c — key binding setup and handling
 *
 * Current bindings:
 *   Alt+F4       — close focused window
 *   Alt+Tab      — cycle focus to next window
 */
#include "wm.h"

#include <stdlib.h>
#include <string.h>
#include <xcb/xcb_keysyms.h>

/* X11 keysym values */
#define XK_F4      0xffc1
#define XK_Tab     0xff09
#define XK_Left    0xff51
#define XK_Up      0xff52
#define XK_Right   0xff53
#define XK_Down    0xff54
#define XK_Super_L 0xffeb

/* Mod4 = Super key */
#define MOD_SUPER  XCB_MOD_MASK_4

static xcb_keycode_t *keysym_to_keycode(xcb_key_symbols_t *syms,
                                        xcb_keysym_t keysym)
{
    return xcb_key_symbols_get_keycode(syms, keysym);
}

static void grab_key(Wm *wm, xcb_keysym_t keysym, uint16_t mod)
{
    xcb_keycode_t *codes = keysym_to_keycode(wm->keysyms, keysym);
    if (!codes) return;
    for (int i = 0; codes[i] != XCB_NO_SYMBOL; i++) {
        xcb_grab_key(wm->conn, 1, wm->root, mod, codes[i],
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        /* Also grab with NumLock (Mod2) and CapsLock */
        xcb_grab_key(wm->conn, 1, wm->root, mod | XCB_MOD_MASK_2,
                     codes[i], XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        xcb_grab_key(wm->conn, 1, wm->root, mod | XCB_MOD_MASK_LOCK,
                     codes[i], XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    }
    free(codes);
}

void wm_keys_setup(Wm *wm)
{
    grab_key(wm, XK_F4,  XCB_MOD_MASK_1);  /* Alt+F4 */
    grab_key(wm, XK_Tab, XCB_MOD_MASK_1);  /* Alt+Tab */

    /* Ctrl+Super+Arrow — desktop navigation */
    uint16_t desk_mod = XCB_MOD_MASK_CONTROL | MOD_SUPER;
    grab_key(wm, XK_Left,  desk_mod);
    grab_key(wm, XK_Right, desk_mod);
    grab_key(wm, XK_Up,    desk_mod);
    grab_key(wm, XK_Down,  desk_mod);

    xcb_flush(wm->conn);
}

static void cycle_focus(Wm *wm)
{
    if (!wm->clients) return;

    if (!wm->focused) {
        wm_focus_client(wm, wm->clients);
        return;
    }

    /* Move to next client, wrapping around */
    WmClient *next = wm->focused->next;
    if (!next) next = wm->clients;
    wm_focus_client(wm, next);
}

void wm_keys_handle(Wm *wm, xcb_key_press_event_t *ev)
{
    xcb_keysym_t sym = xcb_key_symbols_get_keysym(wm->keysyms,
                                                   ev->detail, 0);
    /* Mask out NumLock/CapsLock for comparison */
    uint16_t mod = ev->state & (XCB_MOD_MASK_1 | XCB_MOD_MASK_SHIFT |
                                XCB_MOD_MASK_CONTROL | MOD_SUPER);

    if (sym == XK_F4 && (mod & XCB_MOD_MASK_1)) {
        if (wm->focused)
            wm_close_client(wm, wm->focused);
        return;
    }

    if (sym == XK_Tab && (mod & XCB_MOD_MASK_1)) {
        cycle_focus(wm);
        return;
    }

    /* Ctrl+Super+Arrow — desktop navigation */
    uint16_t desk_mod = XCB_MOD_MASK_CONTROL | MOD_SUPER;
    if ((mod & desk_mod) == desk_mod) {
        if (sym == XK_Left)  { wm_desktops_move(wm, -1,  0); return; }
        if (sym == XK_Right) { wm_desktops_move(wm,  1,  0); return; }
        if (sym == XK_Up)    { wm_desktops_move(wm,  0, -1); return; }
        if (sym == XK_Down)  { wm_desktops_move(wm,  0,  1); return; }
    }
}
