#define _POSIX_C_SOURCE 200809L
/*
 * keys.c — key binding setup and handling
 *
 * Current bindings:
 *   Alt+F4       — close focused window
 *   Alt+Tab      — cycle focus to next window
 *   Shift+Alt+Tab — cycle focus to previous window
 *   Super (tap)  — toggle panel start menu
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
#define XK_l       0x006c

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
    if (!codes) {
        return;
    }
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
    grab_key(wm, XK_Tab, XCB_MOD_MASK_1 | XCB_MOD_MASK_SHIFT); /* Shift+Alt+Tab */

    /* Ctrl+Super+Arrow — desktop navigation */
    uint16_t desk_mod = XCB_MOD_MASK_CONTROL | MOD_SUPER;
    grab_key(wm, XK_Left,  desk_mod);
    grab_key(wm, XK_Right, desk_mod);
    grab_key(wm, XK_Up,    desk_mod);
    grab_key(wm, XK_Down,  desk_mod);

    /* Super+L — lock screen */
    grab_key(wm, XK_l, MOD_SUPER);

    /* Super tap — open start menu */
    grab_key(wm, XK_Super_L, 0);

    xcb_flush(wm->conn);
}

/* Super-tap state: set on Super_L press, cleared on any other key event
 * or modifier press. Fires start-menu toggle on Super_L release when set. */
static int super_tap_armed;

/* Keysym for Alt_L / Alt_R to detect modifier release */
#define XK_Alt_L   0xffe9
#define XK_Alt_R   0xffea
#define XK_Escape  0xff1b

void wm_keys_handle(Wm *wm, xcb_key_press_event_t *ev)
{
    xcb_keysym_t sym = xcb_key_symbols_get_keysym(wm->keysyms,
                                                   ev->detail, 0);
    /* Mask out NumLock/CapsLock for comparison */
    uint16_t mod = ev->state & (XCB_MOD_MASK_1 | XCB_MOD_MASK_SHIFT |
                                XCB_MOD_MASK_CONTROL | MOD_SUPER);

    /* Super tap detection: arm on lone Super_L press, disarm on any other
     * key. The grab only matches when no modifiers are held. */
    if (sym == XK_Super_L && mod == 0) {
        super_tap_armed = 1;
    } else {
        super_tap_armed = 0;
    }

    /* Escape cancels the window switcher */
    if (sym == XK_Escape && wm->switcher_active) {
        wm_switcher_cancel(wm);
        return;
    }

    if (sym == XK_F4 && (mod & XCB_MOD_MASK_1)) {
        if (wm->focused) {
            wm_close_client(wm, wm->focused);
        }
        return;
    }

    if (sym == XK_Tab && (mod & XCB_MOD_MASK_1)) {
        if (wm->switcher_active) {
            if (mod & XCB_MOD_MASK_SHIFT)
                wm_switcher_prev(wm);
            else
                wm_switcher_next(wm);
        } else {
            if (mod & XCB_MOD_MASK_SHIFT)
                wm_switcher_show(wm);  /* show starts at index 1 anyway */
            else
                wm_switcher_show(wm);
        }
        return;
    }

    /* Super+L — lock screen */
    if (sym == XK_l && (mod & MOD_SUPER)) {
        isde_ipc_send(wm->ipc, ISDE_CMD_LOCK, 0, 0, 0, 0);
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

void wm_keys_handle_release(Wm *wm, xcb_key_release_event_t *ev)
{
    xcb_keysym_t sym = xcb_key_symbols_get_keysym(wm->keysyms,
                                                   ev->detail, 0);

    /* Super_L released while armed = tap — toggle start menu */
    if (sym == XK_Super_L && super_tap_armed) {
        super_tap_armed = 0;
        isde_ipc_send(wm->ipc, ISDE_CMD_TOGGLE_START_MENU, 0, 0, 0, 0);
        return;
    }

    if (!wm->switcher_active) return;

    /* Commit the selection when Alt is released */
    if (sym == XK_Alt_L || sym == XK_Alt_R) {
        wm_switcher_commit(wm);
    }
}
