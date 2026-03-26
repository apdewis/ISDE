#define _POSIX_C_SOURCE 200809L
/*
 * wm.c — core window manager: Xt initialization, event loop, client management
 */
#include "wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb_aux.h>

/* ---------- helpers ---------- */

static xcb_atom_t intern(xcb_connection_t *c, const char *name)
{
    xcb_intern_atom_cookie_t ck = xcb_intern_atom(c, 0, strlen(name), name);
    xcb_intern_atom_reply_t *r  = xcb_intern_atom_reply(c, ck, NULL);
    if (!r) return XCB_ATOM_NONE;
    xcb_atom_t a = r->atom;
    free(r);
    return a;
}

/* ---------- D-Bus settings changed ---------- */

static void wm_on_settings_changed(const char *section, const char *key,
                                    void *user_data)
{
    (void)key;
    Wm *wm = (Wm *)user_data;
    if (strcmp(section, "appearance") == 0 || strcmp(section, "*") == 0) {
        isde_theme_reload();
        for (WmClient *c = wm->clients; c; c = c->next)
            frame_apply_theme(wm, c);
    }
}

/* ---------- initialization ---------- */

int wm_init(Wm *wm, int *argc, char **argv)
{
    memset(wm, 0, sizeof(*wm));

    /* Initialize Xt — this opens the X connection for us */
    char **fallbacks = isde_theme_build_resources();
    wm->toplevel = XtAppInitialize(&wm->app, "ISDE-WM",
                                   NULL, 0,
                                   argc, argv,
                                   fallbacks,
                                   NULL, 0);

    wm->conn = XtDisplay(wm->toplevel);
    if (xcb_connection_has_error(wm->conn)) {
        fprintf(stderr, "isde-wm: cannot connect to X server\n");
        return -1;
    }

    wm->screen = XtScreen(wm->toplevel);
    wm->root = wm->screen->root;

    /* Determine screen number by iterating roots */
    wm->screen_num = 0;
    xcb_screen_iterator_t si = xcb_setup_roots_iterator(
        xcb_get_setup(wm->conn));
    for (int i = 0; si.rem; xcb_screen_next(&si), i++) {
        if (si.data->root == wm->root) {
            wm->screen_num = i;
            break;
        }
    }

    /* Claim window manager role via SubstructureRedirect on root */
    uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                    XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY   |
                    XCB_EVENT_MASK_STRUCTURE_NOTIFY      |
                    XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_void_cookie_t ck = xcb_change_window_attributes_checked(
        wm->conn, wm->root, XCB_CW_EVENT_MASK, &mask);
    xcb_generic_error_t *err = xcb_request_check(wm->conn, ck);
    if (err) {
        fprintf(stderr, "isde-wm: another window manager is running\n");
        free(err);
        return -1;
    }

    /* Intern atoms */
    wm->atom_wm_protocols      = intern(wm->conn, "WM_PROTOCOLS");
    wm->atom_wm_delete_window  = intern(wm->conn, "WM_DELETE_WINDOW");
    wm->atom_wm_take_focus     = intern(wm->conn, "WM_TAKE_FOCUS");
    wm->atom_wm_name           = intern(wm->conn, "WM_NAME");
    wm->atom_net_wm_name       = intern(wm->conn, "_NET_WM_NAME");

    /* Load initial colour scheme */
    isde_theme_current();

    /* EWMH, IPC, and D-Bus */
    wm->ewmh = isde_ewmh_init(wm->conn, wm->screen_num);
    wm->ipc  = isde_ipc_init(wm->conn, wm->screen_num);
    wm->dbus = isde_dbus_init();
    if (wm->dbus)
        isde_dbus_settings_subscribe(wm->dbus, wm_on_settings_changed, wm);

    /* Key bindings */
    wm->keysyms = xcb_key_symbols_alloc(wm->conn);
    wm_keys_setup(wm);

    /* EWMH setup (advertise _NET_SUPPORTED, etc.) */
    wm_ewmh_setup(wm);

    /* Manage any pre-existing windows */
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(
        wm->conn, xcb_query_tree(wm->conn, wm->root), NULL);
    if (tree) {
        xcb_window_t *children = xcb_query_tree_children(tree);
        int n = xcb_query_tree_children_length(tree);
        for (int i = 0; i < n; i++) {
            xcb_get_window_attributes_reply_t *attr =
                xcb_get_window_attributes_reply(
                    wm->conn,
                    xcb_get_window_attributes(wm->conn, children[i]),
                    NULL);
            if (attr) {
                if (!attr->override_redirect &&
                    attr->map_state == XCB_MAP_STATE_VIEWABLE) {
                    WmClient *c = frame_create(wm, children[i]);
                    if (c) {
                        XtPopup(c->shell, XtGrabNone);
                        xcb_map_window(wm->conn, c->client);
                    }
                }
                free(attr);
            }
        }
        free(tree);
    }

    wm_ewmh_update_client_list(wm);
    xcb_flush(wm->conn);
    wm->running = 1;
    return 0;
}

/* ---------- client lookup ---------- */

WmClient *wm_find_client_by_frame(Wm *wm, xcb_window_t frame)
{
    for (WmClient *c = wm->clients; c; c = c->next)
        if (c->shell && XtWindow(c->shell) == frame) return c;
    return NULL;
}

WmClient *wm_find_client_by_widget(Wm *wm, Widget w)
{
    for (WmClient *c = wm->clients; c; c = c->next)
        if (c->shell == w || c->title_label == w ||
            c->minimize_btn == w || c->maximize_btn == w ||
            c->close_btn == w)
            return c;
    return NULL;
}

WmClient *wm_find_client_by_window(Wm *wm, xcb_window_t win)
{
    for (WmClient *c = wm->clients; c; c = c->next)
        if (c->client == win) return c;
    return NULL;
}

/* ---------- focus ---------- */

void wm_focus_client(Wm *wm, WmClient *c)
{
    WmClient *prev = wm->focused;
    wm->focused = c;

    if (prev && prev != c) {
        prev->focused = 0;
        frame_apply_theme(wm, prev);
        frame_update_title(wm, prev);
    }

    if (c) {
        c->focused = 1;
        xcb_set_input_focus(wm->conn, XCB_INPUT_FOCUS_POINTER_ROOT,
                            c->client, XCB_CURRENT_TIME);
        /* Raise frame */
        uint32_t vals[] = { XCB_STACK_MODE_ABOVE };
        xcb_configure_window(wm->conn, XtWindow(c->shell),
                             XCB_CONFIG_WINDOW_STACK_MODE, vals);
        frame_apply_theme(wm, c);
        frame_update_title(wm, c);
    }
    wm_ewmh_update_active(wm);
    xcb_flush(wm->conn);
}

/* ---------- remove client ---------- */

void wm_remove_client(Wm *wm, WmClient *c)
{
    WmClient **pp = &wm->clients;
    while (*pp && *pp != c) pp = &(*pp)->next;
    if (*pp) *pp = c->next;

    if (wm->focused == c)
        wm->focused = NULL;
    if (wm->drag_client == c) {
        wm->drag_client = NULL;
        wm->drag_mode = DRAG_NONE;
    }

    frame_destroy(wm, c);
    wm_ewmh_update_client_list(wm);
    wm_ewmh_update_active(wm);

    if (!wm->focused && wm->clients)
        wm_focus_client(wm, wm->clients);
}

/* ---------- close client via WM_DELETE_WINDOW ---------- */

static int client_supports_protocol(Wm *wm, WmClient *c, xcb_atom_t proto)
{
    xcb_icccm_get_wm_protocols_reply_t reply;
    if (!xcb_icccm_get_wm_protocols_reply(
            wm->conn,
            xcb_icccm_get_wm_protocols(wm->conn, c->client,
                                       wm->atom_wm_protocols),
            &reply, NULL))
        return 0;
    int found = 0;
    for (uint32_t i = 0; i < reply.atoms_len; i++) {
        if (reply.atoms[i] == proto) { found = 1; break; }
    }
    xcb_icccm_get_wm_protocols_reply_wipe(&reply);
    return found;
}

void wm_close_client(Wm *wm, WmClient *c)
{
    if (client_supports_protocol(wm, c, wm->atom_wm_delete_window)) {
        xcb_client_message_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.window = c->client;
        ev.type = wm->atom_wm_protocols;
        ev.format = 32;
        ev.data.data32[0] = wm->atom_wm_delete_window;
        ev.data.data32[1] = XCB_CURRENT_TIME;
        xcb_send_event(wm->conn, 0, c->client,
                       XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
    } else {
        xcb_kill_client(wm->conn, c->client);
    }
    xcb_flush(wm->conn);
}

/* ---------- maximize / minimize ---------- */

void wm_maximize_client(Wm *wm, WmClient *c)
{
    if (c->maximized) {
        /* Restore */
        c->x      = c->save_x;
        c->y      = c->save_y;
        c->width  = c->save_w;
        c->height = c->save_h;
        c->maximized = 0;
    } else {
        /* Save current geometry and maximize */
        c->save_x = c->x;
        c->save_y = c->y;
        c->save_w = c->width;
        c->save_h = c->height;

        c->x = 0;
        c->y = 0;
        c->width  = wm->screen->width_in_pixels - 2 * WM_BORDER_WIDTH;
        c->height = wm->screen->height_in_pixels - WM_TITLE_HEIGHT
                    - WM_BORDER_WIDTH;
        c->maximized = 1;
    }
    frame_configure(wm, c);
    xcb_flush(wm->conn);
}

void wm_minimize_client(Wm *wm, WmClient *c)
{
    /* Placeholder: unmap the frame. A proper implementation would
     * add the window to a taskbar/dock list for restoring later. */
    if (c->shell)
        XtPopdown(c->shell);
    xcb_unmap_window(wm->conn, c->client);
    xcb_flush(wm->conn);

    if (wm->focused == c) {
        wm->focused = NULL;
        /* Focus next available client */
        for (WmClient *n = wm->clients; n; n = n->next) {
            if (n != c) {
                wm_focus_client(wm, n);
                break;
            }
        }
        wm_ewmh_update_active(wm);
    }
}

/* ---------- WM event handlers (non-widget events) ---------- */

static void on_map_request(Wm *wm, xcb_map_request_event_t *ev)
{
    if (wm_find_client_by_window(wm, ev->window))
        return;

    xcb_get_window_attributes_reply_t *attr =
        xcb_get_window_attributes_reply(
            wm->conn,
            xcb_get_window_attributes(wm->conn, ev->window),
            NULL);
    if (attr) {
        if (attr->override_redirect) { free(attr); return; }
        free(attr);
    }

    WmClient *c = frame_create(wm, ev->window);
    if (c) {
        xcb_map_window(wm->conn, ev->window);
        XtPopup(c->shell, XtGrabNone);
        wm_focus_client(wm, c);
        wm_ewmh_update_client_list(wm);
        xcb_flush(wm->conn);
    }
}

static void on_configure_request(Wm *wm, xcb_configure_request_event_t *ev)
{
    WmClient *c = wm_find_client_by_window(wm, ev->window);
    if (c) {
        if (ev->value_mask & XCB_CONFIG_WINDOW_X)
            c->x = ev->x;
        if (ev->value_mask & XCB_CONFIG_WINDOW_Y)
            c->y = ev->y;
        if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH)
            c->width = ev->width;
        if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
            c->height = ev->height;
        frame_configure(wm, c);
    } else {
        uint32_t vals[7];
        int i = 0;
        uint16_t mask = ev->value_mask;
        if (mask & XCB_CONFIG_WINDOW_X)           vals[i++] = ev->x;
        if (mask & XCB_CONFIG_WINDOW_Y)           vals[i++] = ev->y;
        if (mask & XCB_CONFIG_WINDOW_WIDTH)       vals[i++] = ev->width;
        if (mask & XCB_CONFIG_WINDOW_HEIGHT)      vals[i++] = ev->height;
        if (mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) vals[i++] = ev->border_width;
        if (mask & XCB_CONFIG_WINDOW_SIBLING)     vals[i++] = ev->sibling;
        if (mask & XCB_CONFIG_WINDOW_STACK_MODE)  vals[i++] = ev->stack_mode;
        xcb_configure_window(wm->conn, ev->window, mask, vals);
    }
    xcb_flush(wm->conn);
}

static void on_unmap_notify(Wm *wm, xcb_unmap_notify_event_t *ev)
{
    WmClient *c = wm_find_client_by_window(wm, ev->window);
    if (c && c->shell && ev->event == XtWindow(c->shell))
        wm_remove_client(wm, c);
}

static void on_destroy_notify(Wm *wm, xcb_destroy_notify_event_t *ev)
{
    WmClient *c = wm_find_client_by_window(wm, ev->window);
    if (c)
        wm_remove_client(wm, c);
}

static void on_button_release(Wm *wm, xcb_button_release_event_t *ev);

static void on_motion_notify(Wm *wm, xcb_motion_notify_event_t *ev)
{
    WmClient *c = wm->drag_client;
    if (!c || wm->drag_mode != DRAG_MOVE) return;

    /* Coalesce: drain any queued motion events and use the latest one */
    xcb_generic_event_t *next;
    while ((next = xcb_poll_for_queued_event(wm->conn))) {
        uint8_t type = next->response_type & ~0x80;
        if (type == XCB_MOTION_NOTIFY) {
            ev = (xcb_motion_notify_event_t *)next;
            /* keep draining */
        } else {
            /* Put it back — we can't, so just handle it now.
             * Non-motion events during drag are rare. */
            if (type == XCB_BUTTON_RELEASE) {
                on_button_release(wm, (xcb_button_release_event_t *)next);
                free(next);
                return;
            }
            /* For other events, dispatch and continue */
            XtDispatchEvent(next, wm->conn);
            free(next);
            break;
        }
    }

    c->x = wm->drag_orig_x + (ev->root_x - wm->drag_start_x);
    c->y = wm->drag_orig_y + (ev->root_y - wm->drag_start_y);

    /* Use XtMoveWidget to keep Xt internal state in sync */
    XtMoveWidget(c->shell, c->x, c->y);
    xcb_flush(wm->conn);
}

static void on_button_release(Wm *wm, xcb_button_release_event_t *ev)
{
    (void)ev;
    if (wm->drag_mode != DRAG_NONE) {
        xcb_ungrab_pointer(wm->conn, XCB_CURRENT_TIME);
        xcb_flush(wm->conn);
        wm->drag_mode   = DRAG_NONE;
        wm->drag_client = NULL;
    }
}

static void on_property_notify(Wm *wm, xcb_property_notify_event_t *ev)
{
    WmClient *c = wm_find_client_by_window(wm, ev->window);
    if (!c) return;

    if (ev->atom == wm->atom_wm_name || ev->atom == wm->atom_net_wm_name)
        frame_update_title(wm, c);
}

static void on_client_message(Wm *wm, xcb_client_message_event_t *ev)
{
    xcb_ewmh_connection_t *ewmh = isde_ewmh_connection(wm->ewmh);

    if (ev->type == ewmh->_NET_ACTIVE_WINDOW) {
        /* Panel or another client is requesting we activate a window */
        WmClient *c = wm_find_client_by_window(wm, ev->window);
        if (!c) return;

        /* If minimized (shell not mapped), restore it */
        if (c->shell && !XtIsRealized(c->shell)) {
            XtRealizeWidget(c->shell);
        }
        /* Re-map frame and client */
        XtPopup(c->shell, XtGrabNone);
        xcb_map_window(wm->conn, c->client);

        wm_focus_client(wm, c);
    } else if (ev->type == ewmh->_NET_CLOSE_WINDOW) {
        WmClient *c = wm_find_client_by_window(wm, ev->window);
        if (c)
            wm_close_client(wm, c);
    }
}

/* ---------- event loop ---------- */

static int is_wm_event(Wm *wm, uint8_t type)
{
    switch (type) {
    case XCB_MAP_REQUEST:
    case XCB_CONFIGURE_REQUEST:
    case XCB_UNMAP_NOTIFY:
    case XCB_DESTROY_NOTIFY:
    case XCB_CLIENT_MESSAGE:
        return 1;
    case XCB_MOTION_NOTIFY:
    case XCB_BUTTON_RELEASE:
        /* Only handle these when a drag is active;
         * otherwise let Xt dispatch to widget callbacks */
        return wm->drag_mode != DRAG_NONE;
    default:
        return 0;
    }
}

void wm_run(Wm *wm)
{
    while (wm->running) {
        /* Dispatch pending D-Bus messages */
        if (wm->dbus)
            isde_dbus_dispatch(wm->dbus);

        /* Process all pending Xt events (widget callbacks, timers, etc.) */
        while (XtAppPending(wm->app)) {
            XtAppProcessEvent(wm->app, XtIMAll);

            /* After Xt processes, poll for WM-specific events that Xt
             * may have queued but can't dispatch (no widget for root) */
            xcb_generic_event_t *ev;
            while ((ev = xcb_poll_for_queued_event(wm->conn))) {
                uint8_t type = ev->response_type & ~0x80;

                /* Check IPC */
                uint32_t cmd;
                if (isde_ipc_decode(wm->ipc, ev, &cmd,
                                    NULL, NULL, NULL, NULL)) {
                    if (cmd == ISDE_CMD_QUIT || cmd == ISDE_CMD_LOGOUT)
                        wm->running = 0;
                    free(ev);
                    continue;
                }

                switch (type) {
                case XCB_MAP_REQUEST:
                    on_map_request(wm, (xcb_map_request_event_t *)ev);
                    break;
                case XCB_CONFIGURE_REQUEST:
                    on_configure_request(wm,
                        (xcb_configure_request_event_t *)ev);
                    break;
                case XCB_UNMAP_NOTIFY:
                    on_unmap_notify(wm, (xcb_unmap_notify_event_t *)ev);
                    break;
                case XCB_DESTROY_NOTIFY:
                    on_destroy_notify(wm, (xcb_destroy_notify_event_t *)ev);
                    break;
                case XCB_CLIENT_MESSAGE:
                    on_client_message(wm, (xcb_client_message_event_t *)ev);
                    break;
                case XCB_MOTION_NOTIFY:
                    on_motion_notify(wm, (xcb_motion_notify_event_t *)ev);
                    break;
                case XCB_BUTTON_RELEASE:
                    on_button_release(wm, (xcb_button_release_event_t *)ev);
                    break;
                case XCB_PROPERTY_NOTIFY:
                    on_property_notify(wm,
                        (xcb_property_notify_event_t *)ev);
                    break;
                case XCB_KEY_PRESS:
                    wm_keys_handle(wm, (xcb_key_press_event_t *)ev);
                    break;
                default:
                    /* Let Xt handle via XtDispatchEvent */
                    XtDispatchEvent(ev, wm->conn);
                    break;
                }
                free(ev);
            }
        }

        /* Block waiting for next event from X server */
        xcb_generic_event_t *ev = xcb_wait_for_event(wm->conn);
        if (!ev) break;

        uint8_t type = ev->response_type & ~0x80;

        uint32_t cmd;
        if (isde_ipc_decode(wm->ipc, ev, &cmd, NULL, NULL, NULL, NULL)) {
            if (cmd == ISDE_CMD_QUIT || cmd == ISDE_CMD_LOGOUT)
                wm->running = 0;
            free(ev);
            continue;
        }

        if (is_wm_event(wm, type)) {
            /* Handle WM events directly */
            switch (type) {
            case XCB_MAP_REQUEST:
                on_map_request(wm, (xcb_map_request_event_t *)ev);
                break;
            case XCB_CONFIGURE_REQUEST:
                on_configure_request(wm,
                    (xcb_configure_request_event_t *)ev);
                break;
            case XCB_UNMAP_NOTIFY:
                on_unmap_notify(wm, (xcb_unmap_notify_event_t *)ev);
                break;
            case XCB_DESTROY_NOTIFY:
                on_destroy_notify(wm, (xcb_destroy_notify_event_t *)ev);
                break;
            case XCB_CLIENT_MESSAGE:
                on_client_message(wm, (xcb_client_message_event_t *)ev);
                break;
            case XCB_MOTION_NOTIFY:
                on_motion_notify(wm, (xcb_motion_notify_event_t *)ev);
                break;
            case XCB_BUTTON_RELEASE:
                on_button_release(wm, (xcb_button_release_event_t *)ev);
                break;
            }
        } else if (type == XCB_PROPERTY_NOTIFY) {
            on_property_notify(wm, (xcb_property_notify_event_t *)ev);
        } else if (type == XCB_KEY_PRESS) {
            wm_keys_handle(wm, (xcb_key_press_event_t *)ev);
        } else {
            /* Widget events — let Xt dispatch to callbacks */
            XtDispatchEvent(ev, wm->conn);
        }

        free(ev);
    }
}

/* ---------- cleanup ---------- */

void wm_cleanup(Wm *wm)
{
    while (wm->clients)
        wm_remove_client(wm, wm->clients);

    if (wm->keysyms)
        xcb_key_symbols_free(wm->keysyms);
    isde_dbus_free(wm->dbus);
    isde_ipc_free(wm->ipc);
    isde_ewmh_free(wm->ewmh);

    XtDestroyApplicationContext(wm->app);
}
