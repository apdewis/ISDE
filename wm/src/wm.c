#define _POSIX_C_SOURCE 200809L
/*
 * wm.c — core window manager: XCB initialization, event loop, client management
 */
#include "wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <poll.h>
#include <time.h>
#include <xcb/xcb_aux.h>

#include <isde/isde-theme.h>
#include <xcb/xcb_cursor.h>
#include <xcb/xcb_xrm.h>
#include "isde/isde-config.h"

static Wm *wm_instance;

/* Determine the HiDPI scale factor the same way libISW does, so the WM's
 * decorations match the rest of the desktop: ISW_SCALE_FACTOR env override,
 * else Xft.dpi / 96 from the X resource database, clamped to >= 1.0. */
static double compute_scale_factor(xcb_connection_t *conn)
{
    const char *env = getenv("ISW_SCALE_FACTOR");
    double scale = 0.0;

    if (env) {
        scale = atof(env);
    }

    if (scale <= 0.0) {
        xcb_xrm_database_t *db = xcb_xrm_database_from_default(conn);
        if (db) {
            char *value = NULL;
            if (xcb_xrm_resource_get_string(db, "Xft.dpi", "Xft.Dpi",
                                            &value) >= 0 && value) {
                double dpi = atof(value);
                if (dpi > 0.0) {
                    scale = dpi / 96.0;
                }
                free(value);
            }
            xcb_xrm_database_free(db);
        }
    }

    if (scale < 1.0) {
        scale = 1.0;
    }
    return scale;
}

/* ---------- helpers ---------- */

static xcb_atom_t intern(xcb_connection_t *c, const char *name)
{
    xcb_intern_atom_cookie_t ck = xcb_intern_atom(c, 0, strlen(name), name);
    xcb_intern_atom_reply_t *r  = xcb_intern_atom_reply(c, ck, NULL);
    if (!r) {
        return XCB_ATOM_NONE;
    }
    xcb_atom_t a = r->atom;
    free(r);
    return a;
}

/* ---------- timer system ---------- */

uint64_t wm_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int wm_timer_add(Wm *wm, uint32_t ms, WmTimerCallback cb, void *data)
{
    for (int i = 0; i < WM_MAX_TIMERS; i++) {
        if (!wm->timers[i].active) {
            wm->timers[i].deadline_ms = wm_now_ms() + ms;
            wm->timers[i].callback = cb;
            wm->timers[i].data = data;
            wm->timers[i].active = 1;
            return i;
        }
    }
    return -1;
}

void wm_timer_remove(Wm *wm, int id)
{
    if (id >= 0 && id < WM_MAX_TIMERS) {
        wm->timers[id].active = 0;
    }
}

static void wm_timers_fire(Wm *wm)
{
    uint64_t now = wm_now_ms();
    for (int i = 0; i < WM_MAX_TIMERS; i++) {
        if (wm->timers[i].active && now >= wm->timers[i].deadline_ms) {
            wm->timers[i].active = 0;
            wm->timers[i].callback(wm->timers[i].data);
        }
    }
}

int wm_timer_next_timeout(Wm *wm)
{
    uint64_t now = wm_now_ms();
    int min_ms = 50;
    for (int i = 0; i < WM_MAX_TIMERS; i++) {
        if (wm->timers[i].active) {
            int64_t remain = (int64_t)(wm->timers[i].deadline_ms - now);
            if (remain <= 0) {
                return 0;
            }
            if ((int)remain < min_ms) {
                min_ms = (int)remain;
            }
        }
    }
    return min_ms;
}

/* ---------- D-Bus settings changed ---------- */

static void wm_on_settings_changed(const char *section, const char *key,
                                    void *user_data)
{
    (void)key;
    Wm *wm = (Wm *)user_data;
    if (strcmp(section, "appearance") == 0 ||
        strcmp(section, "wm.desktops") == 0 ||
        strcmp(section, "*") == 0) {
        wm->running = 0;
        wm->restart = 1;
    }
}

/* ---------- dock window tracking ---------- */
static void wm_add_dock(Wm *wm, xcb_window_t win);
static void wm_remove_dock(Wm *wm, xcb_window_t win);

/* ---------- monitor geometry ---------- */
static void query_monitors(Wm *wm);
static int  monitor_at_point(Wm *wm, int rx, int ry);
static int  monitor_for_client(Wm *wm, WmClient *c);
static void wm_get_monitor_work_area(Wm *wm, int monitor,
                                      int *wx, int *wy, int *ww, int *wh);

/* ---------- initialization ---------- */

int wm_init(Wm *wm, int *argc, char **argv)
{
    (void)argc;
    (void)argv;
    int replace = wm->replace;
    memset(wm, 0, sizeof(*wm));
    wm->replace = replace;

    /* Connect to X server */
    wm->conn = xcb_connect(NULL, &wm->screen_num);
    if (xcb_connection_has_error(wm->conn)) {
        fprintf(stderr, "isde-wm: cannot connect to X server\n");
        return -1;
    }

    /* Find our screen */
    xcb_screen_iterator_t si = xcb_setup_roots_iterator(
        xcb_get_setup(wm->conn));
    for (int i = 0; i < wm->screen_num && si.rem; i++) {
        xcb_screen_next(&si);
    }
    wm->screen = si.data;
    wm->root = wm->screen->root;

    wm->scale_factor = compute_scale_factor(wm->conn);
    if (wm->scale_factor != 1.0) {
        fprintf(stderr, "isde-wm: HiDPI scale factor: %.2f\n",
                wm->scale_factor);
    }
    wm->title_height = wm_scale(wm, WM_TITLE_HEIGHT);

    /* ICCCM WM_Sn selection — WM replacement protocol */
    {
        char sn_name[16];
        snprintf(sn_name, sizeof(sn_name), "WM_S%d", wm->screen_num);
        wm->atom_wm_sn = intern(wm->conn, sn_name);

        xcb_get_selection_owner_reply_t *so =
            xcb_get_selection_owner_reply(wm->conn,
                xcb_get_selection_owner(wm->conn, wm->atom_wm_sn), NULL);
        xcb_window_t old_owner = so ? so->owner : XCB_WINDOW_NONE;
        free(so);

        if (old_owner != XCB_WINDOW_NONE) {
            if (!wm->replace) {
                fprintf(stderr, "isde-wm: another window manager owns %s "
                                "(use --replace)\n", sn_name);
                return -1;
            }

            uint32_t emask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
            xcb_change_window_attributes(wm->conn, old_owner,
                                          XCB_CW_EVENT_MASK, &emask);
        }

        wm->wm_sn_owner = xcb_generate_id(wm->conn);
        xcb_create_window(wm->conn, XCB_COPY_FROM_PARENT,
                          wm->wm_sn_owner, wm->root,
                          -1, -1, 1, 1, 0,
                          XCB_WINDOW_CLASS_INPUT_ONLY,
                          wm->screen->root_visual, 0, NULL);

        xcb_set_selection_owner(wm->conn, wm->wm_sn_owner,
                                wm->atom_wm_sn, XCB_CURRENT_TIME);
        xcb_flush(wm->conn);

        so = xcb_get_selection_owner_reply(wm->conn,
            xcb_get_selection_owner(wm->conn, wm->atom_wm_sn), NULL);
        if (!so || so->owner != wm->wm_sn_owner) {
            fprintf(stderr, "isde-wm: failed to acquire %s selection\n",
                    sn_name);
            free(so);
            return -1;
        }
        free(so);

        xcb_client_message_event_t cm;
        memset(&cm, 0, sizeof(cm));
        cm.response_type = XCB_CLIENT_MESSAGE;
        cm.window = wm->root;
        cm.type = intern(wm->conn, "MANAGER");
        cm.format = 32;
        cm.data.data32[0] = XCB_CURRENT_TIME;
        cm.data.data32[1] = wm->atom_wm_sn;
        cm.data.data32[2] = wm->wm_sn_owner;
        xcb_send_event(wm->conn, 0, wm->root,
                       XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&cm);
        xcb_flush(wm->conn);

        if (old_owner != XCB_WINDOW_NONE) {
            fprintf(stderr, "isde-wm: waiting for old WM to exit...\n");
            for (int timeout = 0; timeout < 100; timeout++) {
                xcb_generic_event_t *ev = xcb_poll_for_event(wm->conn);
                if (!ev) {
                    struct pollfd pfd = { .fd = xcb_get_file_descriptor(wm->conn),
                                          .events = POLLIN };
                    poll(&pfd, 1, 100);
                    continue;
                }
                uint8_t t = ev->response_type & ~0x80;
                if (t == XCB_DESTROY_NOTIFY) {
                    xcb_destroy_notify_event_t *dn =
                        (xcb_destroy_notify_event_t *)ev;
                    if (dn->window == old_owner) {
                        free(ev);
                        break;
                    }
                }
                free(ev);
            }
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
        fprintf(stderr, "isde-wm: another window manager is running "
                        "(SubstructureRedirect failed)\n");
        free(err);
        return -1;
    }

    /* Intern atoms */
    wm->atom_wm_protocols      = intern(wm->conn, "WM_PROTOCOLS");
    wm->atom_wm_delete_window  = intern(wm->conn, "WM_DELETE_WINDOW");
    wm->atom_wm_take_focus     = intern(wm->conn, "WM_TAKE_FOCUS");
    wm->atom_wm_name           = intern(wm->conn, "WM_NAME");
    wm->atom_net_wm_name       = intern(wm->conn, "_NET_WM_NAME");
    wm->atom_motif_wm_hints    = intern(wm->conn, "_MOTIF_WM_HINTS");
    wm->atom_wm_change_state   = intern(wm->conn, "WM_CHANGE_STATE");
    wm->atom_wm_icon_name      = intern(wm->conn, "WM_ICON_NAME");
    wm->atom_net_wm_icon_name  = intern(wm->conn, "_NET_WM_ICON_NAME");
    wm->atom_net_wm_user_time  = intern(wm->conn, "_NET_WM_USER_TIME");
    wm->atom_net_wm_user_time_window = intern(wm->conn, "_NET_WM_USER_TIME_WINDOW");
    wm->atom_net_wm_state_focused = intern(wm->conn, "_NET_WM_STATE_FOCUSED");
    wm->atom_net_startup_info_begin = intern(wm->conn, "_NET_STARTUP_INFO_BEGIN");
    wm->atom_net_startup_info = intern(wm->conn, "_NET_STARTUP_INFO");
    wm->atom_net_startup_id = intern(wm->conn, "_NET_STARTUP_ID");

    /* Load initial colour scheme */
    isde_theme_current();

    /* EWMH, IPC, and D-Bus */
    wm->ewmh = isde_ewmh_init(wm->conn, wm->screen_num);
    wm->ipc  = isde_ipc_init(wm->conn, wm->screen_num);
    wm->dbus = isde_dbus_init();
    if (wm->dbus) {
        isde_dbus_settings_subscribe(wm->dbus, wm_on_settings_changed, wm);
    }

    /* Key bindings */
    wm->keysyms = xcb_key_symbols_alloc(wm->conn);
    wm_keys_setup(wm);

    /* EWMH setup (advertise _NET_SUPPORTED, etc.) */
    wm_ewmh_setup(wm);

    /* Monitor geometry (RandR) */
    {
        const xcb_query_extension_reply_t *ext =
            xcb_get_extension_data(wm->conn, &xcb_randr_id);
        if (ext && ext->present) {
            wm->randr_event_base = ext->first_event;
            xcb_randr_select_input(wm->conn, wm->root,
                XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE |
                XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE);
        }
    }
    query_monitors(wm);

    /* Virtual desktops */
    wm_desktops_init(wm);

    /* Load title bar icons */
    frame_init_icons(wm);

    /* Manage any pre-existing windows */
    xcb_grab_server(wm->conn);
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
                    xcb_atom_t wtype = isde_ewmh_get_window_type(
                        wm->ewmh, children[i]);
                    if (wtype == isde_ewmh_connection(wm->ewmh)
                                     ->_NET_WM_WINDOW_TYPE_DOCK) {
                        wm_add_dock(wm, children[i]);
                        uint32_t emask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
                        xcb_change_window_attributes(wm->conn, children[i],
                                                     XCB_CW_EVENT_MASK,
                                                     &emask);
                        free(attr);
                        continue;
                    }
                    WmClient *c = frame_create(wm, children[i], 1);
                    if (c) {
                        c->focus_seq = ++wm->focus_seq;
                        uint32_t desk = isde_ewmh_get_wm_desktop(
                            wm->ewmh, children[i]);
                        if (desk != 0xFFFFFFFF &&
                            desk >= (uint32_t)wm->num_desktops) {
                            desk = wm->current_desktop;
                        }
                        c->desktop = desk;
                        xcb_ewmh_set_wm_desktop(
                            isde_ewmh_connection(wm->ewmh),
                            c->client, c->desktop);

                        xcb_ewmh_get_atoms_reply_t state;
                        if (xcb_ewmh_get_wm_state_reply(
                                isde_ewmh_connection(wm->ewmh),
                                xcb_ewmh_get_wm_state(
                                    isde_ewmh_connection(wm->ewmh),
                                    children[i]),
                                &state, NULL)) {
                            int was_max = 0;
                            xcb_ewmh_connection_t *ec = isde_ewmh_connection(wm->ewmh);
                            for (uint32_t s = 0; s < state.atoms_len; s++) {
                                if (state.atoms[s] == ec->_NET_WM_STATE_MAXIMIZED_VERT ||
                                    state.atoms[s] == ec->_NET_WM_STATE_MAXIMIZED_HORZ) {
                                    was_max = 1;
                                } else if (state.atoms[s] == ec->_NET_WM_STATE_ABOVE) {
                                    c->above = 1;
                                } else if (state.atoms[s] == ec->_NET_WM_STATE_BELOW) {
                                    c->below = 1;
                                } else if (state.atoms[s] == ec->_NET_WM_STATE_MODAL) {
                                    c->modal = 1;
                                } else if (state.atoms[s] == ec->_NET_WM_STATE_STICKY) {
                                    c->sticky = 1;
                                } else if (state.atoms[s] == ec->_NET_WM_STATE_SKIP_TASKBAR) {
                                    c->skip_taskbar = 1;
                                } else if (state.atoms[s] == ec->_NET_WM_STATE_SKIP_PAGER) {
                                    c->skip_pager = 1;
                                } else if (state.atoms[s] == ec->_NET_WM_STATE_DEMANDS_ATTENTION) {
                                    c->demands_attention = 1;
                                }
                            }
                            xcb_ewmh_get_atoms_reply_wipe(&state);
                            if (was_max) {
                                int mon = monitor_for_client(wm, c);
                                int wx, wy, ww, wh;
                                wm_get_monitor_work_area(wm, mon, &wx, &wy, &ww, &wh);
                                c->save_x = wx + ww / 4;
                                c->save_y = wy + wh / 4;
                                c->save_w = ww / 2;
                                c->save_h = wh / 2;
                                c->maximized = 0;
                                wm_maximize_client(wm, c);
                            }
                        }

                        wm_ewmh_set_allowed_actions(wm, c);

                        if (c->fullscreen) {
                            c->fullscreen = 0;
                            wm_fullscreen_client(wm, c, 1);
                        }

                        int visible = (c->desktop == wm->current_desktop ||
                                       c->desktop == 0xFFFFFFFF);
                        if (visible) {
                            xcb_map_window(wm->conn, c->frame);
                            xcb_map_window(wm->conn, c->client);
                            c->mapped = 1;
                        } else {
                            xcb_unmap_window(wm->conn, c->frame);
                        }
                    }
                }
                free(attr);
            }
        }
        free(tree);
    }
    xcb_ungrab_server(wm->conn);

    /* Restore stacking order */
    {
        xcb_window_t prev = XCB_WINDOW_NONE;
        for (WmClient *c = wm->clients; c; c = c->next) {
            if (prev != XCB_WINDOW_NONE) {
                uint32_t v[] = { prev, XCB_STACK_MODE_ABOVE };
                xcb_configure_window(wm->conn, c->frame,
                    XCB_CONFIG_WINDOW_SIBLING |
                    XCB_CONFIG_WINDOW_STACK_MODE, v);
            }
            prev = c->frame;
        }
    }

    for (int i = 0; i < wm->ndocks; i++) {
        uint32_t v[] = { XCB_STACK_MODE_ABOVE };
        xcb_configure_window(wm->conn, wm->docks[i],
                             XCB_CONFIG_WINDOW_STACK_MODE, v);
    }
    wm_restack_above_below(wm);
    wm_ewmh_update_client_list(wm);
    wm_ewmh_update_client_list_stacking(wm);
    xcb_flush(wm->conn);
    wm_instance = wm;
    wm->running = 1;

#ifdef ISDE_COMPOSITOR
    if (wm_compositor_init(wm) != 0) {
        fprintf(stderr, "isde-wm: compositor init failed, "
                        "continuing without compositing\n");
    }
#endif

    return 0;
}

/* ---------- client lookup ---------- */

WmClient *wm_find_client_by_frame(Wm *wm, xcb_window_t frame)
{
    for (WmClient *c = wm->clients; c; c = c->next) {
        if (c->frame == frame) {
            return c;
        }
    }
    return NULL;
}

WmClient *wm_find_client_by_window(Wm *wm, xcb_window_t win)
{
    for (WmClient *c = wm->clients; c; c = c->next) {
        if (c->client == win) {
            return c;
        }
    }
    return NULL;
}

/* ---------- focus ---------- */

static void wm_update_net_wm_state(Wm *wm, WmClient *c);

void wm_focus_client(Wm *wm, WmClient *c, xcb_timestamp_t time)
{
    if (c) {
        for (WmClient *m = wm->clients; m; m = m->next) {
            if (m->modal && m->transient_for == c->client &&
                m->frame && m->mapped) {
                c = m;
                break;
            }
        }
    }

    WmClient *prev = wm->focused;
    wm->focused = c;

    if (prev && prev != c) {
        prev->focused = 0;
        wm_update_net_wm_state(wm, prev);
        if (prev->fullscreen) {
            uint32_t vals[] = { XCB_STACK_MODE_BELOW };
            xcb_configure_window(wm->conn, prev->frame,
                                 XCB_CONFIG_WINDOW_STACK_MODE, vals);
        }
        frame_apply_theme(wm, prev);
        frame_update_title(wm, prev);
    }

    if (c) {
        c->focused = 1;
        c->focus_seq = ++wm->focus_seq;
        xcb_set_input_focus(wm->conn, XCB_INPUT_FOCUS_POINTER_ROOT,
                            c->client, time);
        fprintf(stderr, "isde-wm: focus+raise client 0x%x frame 0x%x\n",
                c->client, (unsigned)c->frame);
        if (wm->ndocks > 0 && !c->above && !c->fullscreen) {
            uint32_t vals[] = { wm->docks[0], XCB_STACK_MODE_BELOW };
            xcb_configure_window(wm->conn, c->frame,
                                 XCB_CONFIG_WINDOW_SIBLING |
                                 XCB_CONFIG_WINDOW_STACK_MODE, vals);
        } else {
            uint32_t vals[] = { XCB_STACK_MODE_ABOVE };
            xcb_configure_window(wm->conn, c->frame,
                                 XCB_CONFIG_WINDOW_STACK_MODE, vals);
        }
        c->demands_attention = 0;
        wm_update_net_wm_state(wm, c);
        frame_apply_theme(wm, c);
        frame_update_title(wm, c);
    }
    wm_ewmh_update_active(wm);
    wm_restack_above_below(wm);
    wm_ewmh_update_client_list_stacking(wm);
}

/* ---------- remove client ---------- */

static void snap_preview_hide(Wm *wm);

void wm_remove_client(Wm *wm, WmClient *c)
{
    WmClient **pp = &wm->clients;
    while (*pp && *pp != c) {
        pp = &(*pp)->next;
    }
    if (*pp) {
        *pp = c->next;
    }

    if (wm->focused == c) {
        wm->focused = NULL;
    }
    if (wm->drag_client == c) {
        snap_preview_hide(wm);
        wm->drag_client = NULL;
        wm->drag_mode = DRAG_NONE;
    }

    if (wm->switcher_active) {
        wm_switcher_cancel(wm);
    }

#ifdef ISDE_COMPOSITOR
    if (wm->compositor && c->frame) {
        wm_compositor_remove_window(wm->compositor, c->frame);
    }
#endif
    char *old_title = c->title ? strdup(c->title) : NULL;
    char *old_icon  = c->icon_name ? strdup(c->icon_name) : NULL;
    frame_destroy(wm, c);
    if (old_title || old_icon) {
        frame_disambiguate_all(wm, old_title, old_icon);
        free(old_title);
        free(old_icon);
    }
    wm_ewmh_update_client_list(wm);
    wm_ewmh_update_client_list_stacking(wm);
    wm_ewmh_update_active(wm);

    if (!wm->focused && wm->clients) {
        WmClient *mru = NULL;
        for (WmClient *p = wm->clients; p; p = p->next) {
            if (!mru || p->focus_seq > mru->focus_seq) {
                mru = p;
            }
        }
        wm_focus_client(wm, mru, XCB_CURRENT_TIME);
    }
}

/* ---------- close client via WM_DELETE_WINDOW ---------- */

static int client_supports_protocol(Wm *wm, WmClient *c, xcb_atom_t proto)
{
    xcb_icccm_get_wm_protocols_reply_t reply;
    if (!xcb_icccm_get_wm_protocols_reply(
            wm->conn,
            xcb_icccm_get_wm_protocols(wm->conn, c->client,
                                       wm->atom_wm_protocols),
            &reply, NULL)) {
        return 0;
    }
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

/* ---------- work area (respects struts) ---------- */

static void check_strut(xcb_ewmh_connection_t *ewmh, xcb_window_t win,
                        int *top, int *bottom, int *left, int *right)
{
    xcb_ewmh_wm_strut_partial_t strut;
    if (xcb_ewmh_get_wm_strut_partial_reply(ewmh,
            xcb_ewmh_get_wm_strut_partial(ewmh, win),
            &strut, NULL)) {
        if ((int)strut.top > *top)       { *top = strut.top; }
        if ((int)strut.bottom > *bottom) { *bottom = strut.bottom; }
        if ((int)strut.left > *left)     { *left = strut.left; }
        if ((int)strut.right > *right)   { *right = strut.right; }
    }
}

void wm_get_work_area(Wm *wm, int *wx, int *wy, int *ww, int *wh)
{
    int top = 0, bottom = 0, left = 0, right = 0;
    xcb_ewmh_connection_t *ewmh = isde_ewmh_connection(wm->ewmh);

    for (WmClient *c = wm->clients; c; c = c->next) {
        check_strut(ewmh, c->client, &top, &bottom, &left, &right);
    }

    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(
        wm->conn, xcb_query_tree(wm->conn, wm->root), NULL);
    if (tree) {
        xcb_window_t *children = xcb_query_tree_children(tree);
        int nchildren = xcb_query_tree_children_length(tree);
        for (int i = 0; i < nchildren; i++) {
            check_strut(ewmh, children[i], &top, &bottom, &left, &right);
        }
        free(tree);
    }

    *wx = left;
    *wy = top;
    *ww = wm->screen->width_in_pixels - left - right;
    *wh = wm->screen->height_in_pixels - top - bottom;
}

void wm_get_primary_monitor(Wm *wm, int *mx, int *my, int *mw, int *mh)
{
    IsdeMonitor pm;
    isde_randr_primary(wm->conn, wm->root, wm->screen, &pm);
    *mx = pm.x;
    *my = pm.y;
    *mw = pm.width;
    *mh = pm.height;
}

static void wm_get_monitor_work_area(Wm *wm, int monitor,
                                      int *wx, int *wy, int *ww, int *wh)
{
    int gwx, gwy, gww, gwh;
    wm_get_work_area(wm, &gwx, &gwy, &gww, &gwh);

    MonitorGeom *m = &wm->monitors[monitor];
    int x0 = m->x > gwx ? m->x : gwx;
    int y0 = m->y > gwy ? m->y : gwy;
    int x1 = (m->x + m->width) < (gwx + gww) ?
             (m->x + m->width) : (gwx + gww);
    int y1 = (m->y + m->height) < (gwy + gwh) ?
             (m->y + m->height) : (gwy + gwh);
    *wx = x0;
    *wy = y0;
    *ww = x1 - x0 > 0 ? x1 - x0 : 0;
    *wh = y1 - y0 > 0 ? y1 - y0 : 0;
}

static int monitor_for_client(Wm *wm, WmClient *c)
{
    int cx = c->x + c->width / 2;
    int cy = c->y + c->height / 2;
    return monitor_at_point(wm, cx, cy);
}

/* ---------- monitor geometry ---------- */

static void query_monitors(Wm *wm)
{
    free(wm->monitors);
    wm->monitors = NULL;
    wm->nmonitors = 0;

    IsdeMonitor *mons = NULL;
    int n = isde_randr_monitors(wm->conn, wm->root, &mons);

    if (n > 0) {
        wm->monitors = mons;
        wm->nmonitors = n;
    } else {
        wm->monitors = malloc(sizeof(MonitorGeom));
        wm->monitors[0].x = 0;
        wm->monitors[0].y = 0;
        wm->monitors[0].width  = wm->screen->width_in_pixels;
        wm->monitors[0].height = wm->screen->height_in_pixels;
        wm->nmonitors = 1;
    }
}

static int monitor_at_point(Wm *wm, int rx, int ry)
{
    for (int i = 0; i < wm->nmonitors; i++) {
        MonitorGeom *m = &wm->monitors[i];
        if (rx >= m->x && rx < m->x + m->width &&
            ry >= m->y && ry < m->y + m->height) {
            return i;
        }
    }
    return 0;
}

static void rescue_orphaned_clients(Wm *wm)
{
    for (WmClient *c = wm->clients; c; c = c->next) {
        int cx = c->x + c->width / 2;
        int cy = c->y + c->height / 2;

        int on_monitor = 0;
        for (int i = 0; i < wm->nmonitors; i++) {
            MonitorGeom *m = &wm->monitors[i];
            if (cx >= m->x && cx < m->x + m->width &&
                cy >= m->y && cy < m->y + m->height) {
                on_monitor = 1;
                break;
            }
        }
        if (on_monitor) {
            continue;
        }

        int mon = 0;
        int best_dist = INT_MAX;
        for (int i = 0; i < wm->nmonitors; i++) {
            MonitorGeom *m = &wm->monitors[i];
            int mx = m->x + m->width / 2;
            int my = m->y + m->height / 2;
            int d = (cx - mx) * (cx - mx) + (cy - my) * (cy - my);
            if (d < best_dist) { best_dist = d; mon = i; }
        }

        MonitorGeom *target = &wm->monitors[mon];
        if (c->maximized) {
            int wx, wy, ww, wh;
            wm_get_monitor_work_area(wm, mon, &wx, &wy, &ww, &wh);
            c->x = wx;
            c->y = wy;
            int title = c->decorated ? wm->title_height : 0;
            c->width  = ww - 2 * WM_BORDER_WIDTH;
            c->height = wh - title - 2 * WM_BORDER_WIDTH;
        } else {
            int fw = frame_total_width(c);
            int fh = frame_total_height(wm, c);
            if (c->x + fw > target->x + target->width) {
                c->x = target->x + target->width - fw;
            }
            if (c->y + fh > target->y + target->height) {
                c->y = target->y + target->height - fh;
            }
            if (c->x < target->x) {
                c->x = target->x;
            }
            if (c->y < target->y) {
                c->y = target->y;
            }
        }

        frame_configure(wm, c);
    }
    xcb_flush(wm->conn);
}

/* ---------- snap detection ---------- */

enum {
    SNAP_NONE = 0,
    SNAP_LEFT, SNAP_RIGHT,
    SNAP_TL, SNAP_TR, SNAP_BL, SNAP_BR
};

#define SNAP_THRESHOLD 8

static int detect_snap_zone(Wm *wm, int rx, int ry)
{
    int mi = monitor_at_point(wm, rx, ry);
    MonitorGeom *m = &wm->monitors[mi];
    int t = SNAP_THRESHOLD;

    int at_left   = (rx <= m->x + t);
    int at_right  = (rx >= m->x + m->width - t - 1);
    int at_top    = (ry <= m->y + t);
    int at_bottom = (ry >= m->y + m->height - t - 1);

    if (at_left  && at_top)    return SNAP_TL;
    if (at_right && at_top)    return SNAP_TR;
    if (at_left  && at_bottom) return SNAP_BL;
    if (at_right && at_bottom) return SNAP_BR;
    if (at_left)               return SNAP_LEFT;
    if (at_right)              return SNAP_RIGHT;
    return SNAP_NONE;
}

static void snap_geometry(Wm *wm, int zone, int monitor,
                           int *sx, int *sy, int *sw, int *sh)
{
    int wx, wy, ww, wh;
    wm_get_monitor_work_area(wm, monitor, &wx, &wy, &ww, &wh);

    if (ww <= 0 || wh <= 0) {
        *sx = *sy = *sw = *sh = 0;
        return;
    }

    switch (zone) {
    case SNAP_LEFT:
        *sx = wx; *sy = wy;
        *sw = ww / 2; *sh = wh;
        break;
    case SNAP_RIGHT:
        *sx = wx + ww / 2; *sy = wy;
        *sw = ww - ww / 2; *sh = wh;
        break;
    case SNAP_TL:
        *sx = wx; *sy = wy;
        *sw = ww / 2; *sh = wh / 2;
        break;
    case SNAP_TR:
        *sx = wx + ww / 2; *sy = wy;
        *sw = ww - ww / 2; *sh = wh / 2;
        break;
    case SNAP_BL:
        *sx = wx; *sy = wy + wh / 2;
        *sw = ww / 2; *sh = wh - wh / 2;
        break;
    case SNAP_BR:
        *sx = wx + ww / 2; *sy = wy + wh / 2;
        *sw = ww - ww / 2; *sh = wh - wh / 2;
        break;
    default:
        *sx = *sy = *sw = *sh = 0;
        break;
    }
}

static void snap_preview_show(Wm *wm, int zone, int monitor)
{
    int lx, ly, lw, lh;
    snap_geometry(wm, zone, monitor, &lx, &ly, &lw, &lh);
    if (lw <= 0 || lh <= 0) { return; }

    int inset = 2;
    lx += inset; ly += inset;
    lw -= 2 * inset; lh -= 2 * inset;
    if (lw < 1) { lw = 1; }
    if (lh < 1) { lh = 1; }

    const IsdeColorScheme *s = isde_theme_current();
    unsigned int color = s ? s->active : 0x4488CC;

    if (!wm->snap_preview) {
        xcb_alloc_color_reply_t *cr = xcb_alloc_color_reply(
            wm->conn,
            xcb_alloc_color(wm->conn, wm->screen->default_colormap,
                            ((color >> 16) & 0xFF) * 257,
                            ((color >> 8)  & 0xFF) * 257,
                            ( color        & 0xFF) * 257),
            NULL);
        uint32_t bg = cr ? cr->pixel : wm->screen->white_pixel;
        free(cr);

        wm->snap_preview = xcb_generate_id(wm->conn);
        uint32_t vals[] = {
            bg,
            1,
            XCB_EVENT_MASK_NO_EVENT
        };
        xcb_create_window(wm->conn, XCB_COPY_FROM_PARENT,
                          wm->snap_preview, wm->root,
                          lx, ly, lw, lh, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          wm->screen->root_visual,
                          XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
                          XCB_CW_EVENT_MASK,
                          vals);
        uint32_t opacity = (uint32_t)(0.5 * 0xFFFFFFFF);
        xcb_atom_t atom_opacity = intern(wm->conn, "_NET_WM_WINDOW_OPACITY");
        xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE,
                            wm->snap_preview, atom_opacity,
                            XCB_ATOM_CARDINAL, 32, 1, &opacity);

        xcb_map_window(wm->conn, wm->snap_preview);
    } else {
        uint32_t cfg[] = { lx, ly, lw, lh };
        xcb_configure_window(wm->conn, wm->snap_preview,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                             cfg);
    }
    xcb_flush(wm->conn);
}

static void snap_preview_hide(Wm *wm)
{
    if (wm->snap_preview) {
        xcb_destroy_window(wm->conn, wm->snap_preview);
        wm->snap_preview = 0;
        xcb_flush(wm->conn);
    }
    wm->snap_pending = SNAP_NONE;
}

static void apply_snap(Wm *wm, WmClient *c, int zone, int monitor)
{
    int sx, sy, sw, sh;
    snap_geometry(wm, zone, monitor, &sx, &sy, &sw, &sh);

    int th = wm->title_height;

    c->save_x = c->x;
    c->save_y = c->y;
    c->save_w = c->width;
    c->save_h = c->height;

    c->x = sx;
    c->y = sy;
    c->width = sw;
    c->height = sh - th;

    frame_configure(wm, c);
    xcb_flush(wm->conn);
}

/* ---------- _NET_WM_STATE helper ---------- */

static void wm_update_net_wm_state(Wm *wm, WmClient *c)
{
    xcb_ewmh_connection_t *ewmh = isde_ewmh_connection(wm->ewmh);
    xcb_atom_t states[12];
    int n = 0;

    if (c->maximized) {
        states[n++] = ewmh->_NET_WM_STATE_MAXIMIZED_VERT;
        states[n++] = ewmh->_NET_WM_STATE_MAXIMIZED_HORZ;
    }
    if (c->fullscreen) {
        states[n++] = ewmh->_NET_WM_STATE_FULLSCREEN;
    }
    if (c->above) {
        states[n++] = ewmh->_NET_WM_STATE_ABOVE;
    }
    if (c->below) {
        states[n++] = ewmh->_NET_WM_STATE_BELOW;
    }
    if (c->modal) {
        states[n++] = ewmh->_NET_WM_STATE_MODAL;
    }
    if (c->sticky) {
        states[n++] = ewmh->_NET_WM_STATE_STICKY;
    }
    if (c->skip_taskbar) {
        states[n++] = ewmh->_NET_WM_STATE_SKIP_TASKBAR;
    }
    if (c->skip_pager) {
        states[n++] = ewmh->_NET_WM_STATE_SKIP_PAGER;
    }
    if (c->demands_attention) {
        states[n++] = ewmh->_NET_WM_STATE_DEMANDS_ATTENTION;
    }
    if (c->focused) {
        states[n++] = wm->atom_net_wm_state_focused;
    }

    xcb_ewmh_set_wm_state(ewmh, c->client, n, n ? states : NULL);
}

/* ---------- maximize / minimize ---------- */

void wm_maximize_client(Wm *wm, WmClient *c)
{
    if (c->maximized) {
        c->x      = c->save_x;
        c->y      = c->save_y;
        c->width  = c->save_w;
        c->height = c->save_h;
        c->maximized = 0;
    } else {
        c->save_x = c->x;
        c->save_y = c->y;
        c->save_w = c->width;
        c->save_h = c->height;

        int mon = monitor_for_client(wm, c);
        int wx, wy, ww, wh;
        wm_get_monitor_work_area(wm, mon, &wx, &wy, &ww, &wh);
        c->x = wx;
        c->y = wy;
        int title = c->decorated ? wm->title_height : 0;
        c->width  = ww - 2 * WM_BORDER_WIDTH;
        c->height = wh - title - 2 * WM_BORDER_WIDTH;
        c->maximized = 1;
    }
    frame_configure(wm, c);

    wm_update_net_wm_state(wm, c);

    xcb_flush(wm->conn);
}

void wm_fullscreen_client(Wm *wm, WmClient *c, int enable)
{
    if (enable && !c->fullscreen) {
        c->save_x = c->x;
        c->save_y = c->y;
        c->save_w = c->width;
        c->save_h = c->height;

        int mon = monitor_for_client(wm, c);
        MonitorGeom *m = &wm->monitors[mon];
        c->x = m->x;
        c->y = m->y;
        c->width = m->width;
        c->height = m->height;
        c->fullscreen = 1;

        uint32_t vals[] = { c->x, c->y, c->width, c->height, 0 };
        xcb_configure_window(wm->conn, c->frame,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                             XCB_CONFIG_WINDOW_BORDER_WIDTH, vals);

        uint32_t cpos[] = { 0, 0 };
        xcb_configure_window(wm->conn, c->client,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, cpos);

        uint32_t cvals[] = { (uint32_t)c->width, (uint32_t)c->height };
        xcb_configure_window(wm->conn, c->client,
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                             cvals);

        uint32_t above[] = { XCB_STACK_MODE_ABOVE };
        xcb_configure_window(wm->conn, c->frame,
                             XCB_CONFIG_WINDOW_STACK_MODE, above);

        xcb_ewmh_set_frame_extents(isde_ewmh_connection(wm->ewmh),
                                   c->client, 0, 0, 0, 0);
    } else if (!enable && c->fullscreen) {
        c->x      = c->save_x;
        c->y      = c->save_y;
        c->width  = c->save_w;
        c->height = c->save_h;
        c->fullscreen = 0;
        frame_configure(wm, c);
    }

    wm_update_net_wm_state(wm, c);
    xcb_flush(wm->conn);
}

void wm_minimize_client(Wm *wm, WmClient *c)
{
    c->minimized = 1;
    if (c->frame) {
        xcb_unmap_window(wm->conn, c->frame);
        c->mapped = 0;
    }

    xcb_unmap_window(wm->conn, c->client);
    xcb_flush(wm->conn);

    if (wm->focused == c) {
        wm->focused = NULL;
        for (WmClient *n = wm->clients; n; n = n->next) {
            if (n != c) {
                wm_focus_client(wm, n, XCB_CURRENT_TIME);
                break;
            }
        }
        wm_ewmh_update_active(wm);
    }
}

void wm_restore_client(Wm *wm, WmClient *c)
{
    if (!c->minimized) return;
    c->minimized = 0;
    xcb_map_window(wm->conn, c->client);
    if (c->frame) {
        xcb_map_window(wm->conn, c->frame);
        c->mapped = 1;
    }
    xcb_flush(wm->conn);
}

/* ---------- above / below / move to desktop ---------- */

void wm_restack_above_below(Wm *wm)
{
    for (WmClient *c = wm->clients; c; c = c->next) {
        if (!c->frame || !c->mapped) {
            continue;
        }
        if (c->below) {
            uint32_t v[] = { XCB_STACK_MODE_BELOW };
            xcb_configure_window(wm->conn, c->frame,
                                 XCB_CONFIG_WINDOW_STACK_MODE, v);
        }
    }
    for (WmClient *c = wm->clients; c; c = c->next) {
        if (!c->frame || !c->mapped) {
            continue;
        }
        if (c->above) {
            uint32_t v[] = { XCB_STACK_MODE_ABOVE };
            xcb_configure_window(wm->conn, c->frame,
                                 XCB_CONFIG_WINDOW_STACK_MODE, v);
        }
    }
    for (WmClient *c = wm->clients; c; c = c->next) {
        if (!c->frame || !c->mapped) {
            continue;
        }
        if (!c->modal || !c->transient_for) {
            continue;
        }
        WmClient *parent = wm_find_client_by_window(wm, c->transient_for);
        if (!parent || !parent->frame || !parent->mapped) {
            continue;
        }
        uint32_t v[] = { parent->frame, XCB_STACK_MODE_ABOVE };
        xcb_configure_window(wm->conn, c->frame,
                             XCB_CONFIG_WINDOW_SIBLING |
                             XCB_CONFIG_WINDOW_STACK_MODE, v);
    }
    xcb_flush(wm->conn);
}

void wm_set_above(Wm *wm, WmClient *c, int enable)
{
    if (enable && c->below) {
        c->below = 0;
    }
    c->above = enable;
    wm_update_net_wm_state(wm, c);
    wm_restack_above_below(wm);
    wm_ewmh_update_client_list_stacking(wm);
}

void wm_set_below(Wm *wm, WmClient *c, int enable)
{
    if (enable && c->above) {
        c->above = 0;
    }
    c->below = enable;
    wm_update_net_wm_state(wm, c);
    wm_restack_above_below(wm);
    wm_ewmh_update_client_list_stacking(wm);
}

void wm_move_to_desktop(Wm *wm, WmClient *c, uint32_t desktop)
{
    if (desktop == c->desktop) {
        return;
    }

    uint32_t old = c->desktop;
    c->desktop = desktop;
    xcb_ewmh_set_wm_desktop(isde_ewmh_connection(wm->ewmh),
                            c->client, desktop);

    int visible_now = (desktop == wm->current_desktop ||
                       desktop == 0xFFFFFFFF);
    int visible_before = (old == wm->current_desktop ||
                          old == 0xFFFFFFFF);

    if (visible_before && !visible_now) {
        c->hidden = 1;
        xcb_unmap_window(wm->conn, c->client);
        if (c->frame && c->mapped) {
            xcb_unmap_window(wm->conn, c->frame);
            c->mapped = 0;
        }
        if (wm->focused == c) {
            wm->focused = NULL;
            wm_ewmh_update_active(wm);
        }
    } else if (!visible_before && visible_now) {
        c->hidden = 0;
        xcb_map_window(wm->conn, c->client);
        if (c->frame) {
            xcb_map_window(wm->conn, c->frame);
            c->mapped = 1;
        }
    }

    xcb_flush(wm->conn);
}

/* ---------- WM event handlers ---------- */

#define MWM_HINTS_DECORATIONS (1 << 1)
int wm_client_wants_decorations(Wm *wm, xcb_window_t win)
{
    xcb_get_property_reply_t *reply = xcb_get_property_reply(
        wm->conn,
        xcb_get_property(wm->conn, 0, win,
                         wm->atom_motif_wm_hints,
                         XCB_ATOM_ANY,
                         0, 5),
        NULL);
    if (!reply) {
        return 1;
    }

    int dominated = 1;
    if (xcb_get_property_value_length(reply) >= 3 * (int)sizeof(uint32_t)) {
        uint32_t *hints = (uint32_t *)xcb_get_property_value(reply);
        uint32_t flags       = hints[0];
        uint32_t decorations = hints[2];
        if ((flags & MWM_HINTS_DECORATIONS) && decorations == 0) {
            dominated = 0;
        }
    }
    free(reply);
    return dominated;
}

int wm_window_type_wants_decorations(Wm *wm, xcb_window_t win)
{
    xcb_ewmh_connection_t *ewmh = isde_ewmh_connection(wm->ewmh);
    xcb_atom_t type = isde_ewmh_get_window_type(wm->ewmh, win);

    if (type == ewmh->_NET_WM_WINDOW_TYPE_DOCK    ||
        type == ewmh->_NET_WM_WINDOW_TYPE_DESKTOP  ||
        type == ewmh->_NET_WM_WINDOW_TYPE_SPLASH   ||
        type == ewmh->_NET_WM_WINDOW_TYPE_TOOLBAR   ||
        type == ewmh->_NET_WM_WINDOW_TYPE_MENU      ||
        type == ewmh->_NET_WM_WINDOW_TYPE_NOTIFICATION) {
        return 0;
    }
    return 1;
}

static void wm_add_dock(Wm *wm, xcb_window_t win)
{
    for (int i = 0; i < wm->ndocks; i++) {
        if (wm->docks[i] == win) {
            return;
        }
    }
    if (wm->ndocks >= wm->cap_docks) {
        wm->cap_docks = wm->cap_docks ? wm->cap_docks * 2 : 4;
        wm->docks = realloc(wm->docks, wm->cap_docks * sizeof(xcb_window_t));
    }
    wm->docks[wm->ndocks++] = win;

    uint32_t v[] = { XCB_STACK_MODE_ABOVE };
    xcb_configure_window(wm->conn, win,
                         XCB_CONFIG_WINDOW_STACK_MODE, v);

    wm_ewmh_update_workarea(wm);
}

static void wm_remove_dock(Wm *wm, xcb_window_t win)
{
    for (int i = 0; i < wm->ndocks; i++) {
        if (wm->docks[i] == win) {
            wm->docks[i] = wm->docks[--wm->ndocks];
            wm_ewmh_update_workarea(wm);
            return;
        }
    }
}

/* ---------- startup notification ---------- */

WmStartupSeq *wm_find_startup_seq(Wm *wm, const char *id)
{
    for (WmStartupSeq *s = wm->startup_seqs; s; s = s->next) {
        if (strcmp(s->id, id) == 0) {
            return s;
        }
    }
    return NULL;
}

WmStartupSeq *wm_find_startup_seq_by_wmclass(Wm *wm, const char *instance,
                                              const char *class_name)
{
    for (WmStartupSeq *s = wm->startup_seqs; s; s = s->next) {
        if (!s->wmclass) {
            continue;
        }
        if ((class_name && strcasecmp(s->wmclass, class_name) == 0) ||
            (instance && strcasecmp(s->wmclass, instance) == 0)) {
            return s;
        }
    }
    return NULL;
}

void wm_remove_startup_seq(Wm *wm, WmStartupSeq *seq)
{
    WmStartupSeq **pp = &wm->startup_seqs;
    while (*pp && *pp != seq) {
        pp = &(*pp)->next;
    }
    if (*pp) {
        *pp = seq->next;
    }

    if (seq->timer_id >= 0) {
        wm_timer_remove(wm, seq->timer_id);
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "remove: ID=%s", seq->id);
    xcb_atom_t begin = isde_ewmh_atom_startup_info_begin(wm->ewmh);
    size_t len = strlen(msg) + 1;
    const char *p = msg;
    int first = 1;
    while (len > 0) {
        xcb_client_message_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.window = wm->root;
        ev.type = first ? begin : isde_ewmh_atom_startup_info(wm->ewmh);
        ev.format = 8;
        size_t chunk = len > 20 ? 20 : len;
        memcpy(ev.data.data8, p, chunk);
        xcb_send_event(wm->conn, 0, wm->root,
                       XCB_EVENT_MASK_PROPERTY_CHANGE,
                       (const char *)&ev);
        p += chunk;
        len -= chunk;
        first = 0;
    }
    xcb_flush(wm->conn);

    free(seq->id);
    free(seq->wmclass);
    free(seq);
}

typedef struct {
    Wm *wm;
    char *id;
} StartupTimeoutData;

static void startup_timeout_cb(void *data)
{
    StartupTimeoutData *td = data;
    if (td->wm) {
        WmStartupSeq *seq = wm_find_startup_seq(td->wm, td->id);
        if (seq) {
            fprintf(stderr, "isde-wm: startup notification timeout for %s\n",
                    seq->id);
            seq->timer_id = -1;
            wm_remove_startup_seq(td->wm, seq);
        }
    }
    free(td->id);
    free(td);
}

static char *sn_parse_value(const char *msg, const char *key)
{
    const char *p = strstr(msg, key);
    if (!p) {
        return NULL;
    }
    p += strlen(key);

    if (*p == '"') {
        p++;
        size_t cap = 128;
        char *val = malloc(cap);
        size_t len = 0;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                p++;
            }
            if (len + 1 >= cap) {
                cap *= 2;
                val = realloc(val, cap);
            }
            val[len++] = *p++;
        }
        val[len] = '\0';
        return val;
    }

    const char *end = p;
    while (*end && *end != ' ' && *end != '\t') {
        end++;
    }
    return strndup(p, end - p);
}

static void sn_handle_message(Wm *wm, const char *msg)
{
    if (strncmp(msg, "new:", 4) == 0) {
        char *id = sn_parse_value(msg, "ID=");
        if (!id) {
            return;
        }

        if (wm_find_startup_seq(wm, id)) {
            free(id);
            return;
        }

        WmStartupSeq *seq = calloc(1, sizeof(*seq));
        seq->id = id;
        seq->wmclass = sn_parse_value(msg, "WMCLASS=");
        seq->timer_id = -1;

        char *ts_str = sn_parse_value(msg, "TIMESTAMP=");
        if (ts_str) {
            seq->timestamp = (uint32_t)strtoul(ts_str, NULL, 10);
            free(ts_str);
        }

        StartupTimeoutData *td = malloc(sizeof(*td));
        td->wm = wm;
        td->id = strdup(id);
        seq->timer_id = wm_timer_add(wm, STARTUP_TIMEOUT_MS,
                                      startup_timeout_cb, td);

        seq->next = wm->startup_seqs;
        wm->startup_seqs = seq;

        fprintf(stderr, "isde-wm: startup notification new: %s wmclass=%s\n",
                id, seq->wmclass ? seq->wmclass : "(none)");

    } else if (strncmp(msg, "remove:", 7) == 0) {
        char *id = sn_parse_value(msg, "ID=");
        if (!id) {
            return;
        }
        WmStartupSeq *seq = wm_find_startup_seq(wm, id);
        if (seq) {
            fprintf(stderr, "isde-wm: startup notification remove: %s\n", id);
            wm_remove_startup_seq(wm, seq);
        }
        free(id);
    }
}

static void on_startup_info(Wm *wm, xcb_client_message_event_t *ev)
{
    int is_begin = (ev->type == wm->atom_net_startup_info_begin);

    if (is_begin) {
        wm->sn_buf_len = 0;
    }

    int space = (int)sizeof(wm->sn_buf) - wm->sn_buf_len - 1;
    if (space <= 0) {
        wm->sn_buf_len = 0;
        return;
    }

    int chunk = 20;
    if (chunk > space) {
        chunk = space;
    }
    memcpy(wm->sn_buf + wm->sn_buf_len, ev->data.data8, chunk);
    wm->sn_buf_len += chunk;
    wm->sn_buf[wm->sn_buf_len] = '\0';

    if (memchr(ev->data.data8, '\0', 20)) {
        sn_handle_message(wm, wm->sn_buf);
        wm->sn_buf_len = 0;
    }
}

static void on_map_request(Wm *wm, xcb_map_request_event_t *ev)
{
    fprintf(stderr, "isde-wm: MapRequest for window 0x%x\n", ev->window);

    if (wm_find_client_by_window(wm, ev->window)) {
        return;
    }

    xcb_get_window_attributes_reply_t *attr =
        xcb_get_window_attributes_reply(
            wm->conn,
            xcb_get_window_attributes(wm->conn, ev->window),
            NULL);
    if (attr) {
        fprintf(stderr, "isde-wm:   override_redirect=%d map_state=%d\n",
                attr->override_redirect, attr->map_state);
        if (attr->override_redirect) {
            free(attr);
            return;
        }
        free(attr);
    }

    xcb_atom_t type = isde_ewmh_get_window_type(wm->ewmh, ev->window);
    if (type == isde_ewmh_connection(wm->ewmh)->_NET_WM_WINDOW_TYPE_DOCK) {
        fprintf(stderr, "isde-wm:   dock window — not managing\n");
        wm_add_dock(wm, ev->window);
        uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
        xcb_change_window_attributes(wm->conn, ev->window,
                                     XCB_CW_EVENT_MASK, &mask);
        xcb_map_window(wm->conn, ev->window);
        wm_restack_above_below(wm);
        xcb_flush(wm->conn);
        return;
    }

    WmClient *c = frame_create(wm, ev->window, 0);
    if (c) {
        c->desktop = wm->current_desktop;
        xcb_ewmh_set_wm_desktop(isde_ewmh_connection(wm->ewmh),
                                c->client, c->desktop);
        wm_ewmh_set_allowed_actions(wm, c);
        xcb_map_window(wm->conn, ev->window);
        xcb_map_window(wm->conn, c->frame);
        c->mapped = 1;
        /* The compositor tracks the frame via the MapNotify it receives
         * on root — no explicit add needed here. */
        if (c->fullscreen) {
            c->fullscreen = 0;
            wm_fullscreen_client(wm, c, 1);
        }
        int dominated_focus = 1;
        WmStartupSeq *matched_seq = NULL;

        if (c->startup_id) {
            matched_seq = wm_find_startup_seq(wm, c->startup_id);
        }
        if (!matched_seq) {
            char *inst = NULL, *cls = NULL;
            if (isde_ewmh_get_wm_class(wm->ewmh, ev->window, &inst, &cls)) {
                matched_seq = wm_find_startup_seq_by_wmclass(wm, inst, cls);
                free(inst);
                free(cls);
            }
        }

        if (matched_seq) {
            if (matched_seq->timestamp != 0) {
                c->user_time = matched_seq->timestamp;
            }
            dominated_focus = 1;
            wm_remove_startup_seq(wm, matched_seq);
        } else {
            if (wm->focused && c->user_time != 0 && wm->last_user_time != 0 &&
                (int32_t)(c->user_time - wm->last_user_time) < 0) {
                dominated_focus = 0;
            }
        }

        if (dominated_focus) {
            wm_focus_client(wm, c, c->user_time ? c->user_time : XCB_CURRENT_TIME);
        }
        if (c->above || c->below) {
            wm_restack_above_below(wm);
        }
        wm_ewmh_update_client_list(wm);
        wm_ewmh_update_client_list_stacking(wm);
        xcb_flush(wm->conn);
    }
}

static WmClient *find_grip_client(Wm *wm, xcb_window_t win, int *edge)
{
    for (WmClient *c = wm->clients; c; c = c->next) {
        for (int i = 0; i < 8; i++) {
            if (c->grip[i] == win) { *edge = i; return c; }
        }
    }
    *edge = -1;
    return NULL;
}

static void on_grip_press(Wm *wm, xcb_button_press_event_t *ev)
{
    int edge;
    WmClient *c = find_grip_client(wm, ev->event, &edge);
    if (!c || edge < 0 || c->fixed_size) {
        return;
    }

    wm_focus_client(wm, c, ev->time);
    wm->drag_mode    = DRAG_RESIZE;
    wm->resize_edge  = edge;
    wm->drag_client  = c;
    wm->drag_start_x = ev->root_x;
    wm->drag_start_y = ev->root_y;
    wm->drag_orig_x  = c->x;
    wm->drag_orig_y  = c->y;
    wm->drag_orig_w  = c->width;
    wm->drag_orig_h  = c->height;

    xcb_grab_pointer(wm->conn, 1, wm->root,
                     XCB_EVENT_MASK_BUTTON_RELEASE |
                     XCB_EVENT_MASK_POINTER_MOTION,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                     XCB_NONE, wm->cursors[edge], XCB_CURRENT_TIME);
    xcb_flush(wm->conn);
}

static void on_configure_request(Wm *wm, xcb_configure_request_event_t *ev)
{
    WmClient *c = wm_find_client_by_window(wm, ev->window);
    if (c) {
        if (ev->value_mask & XCB_CONFIG_WINDOW_X) {
            c->x = ev->x;
        }
        if (ev->value_mask & XCB_CONFIG_WINDOW_Y) {
            c->y = ev->y;
        }
        if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
            c->width = ev->width;
        }
        if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
            c->height = ev->height;
        }
        frame_configure(wm, c);
    } else {
        uint32_t vals[7];
        int i = 0;
        uint16_t mask = ev->value_mask;
        if (mask & XCB_CONFIG_WINDOW_X)           { vals[i++] = ev->x; }
        if (mask & XCB_CONFIG_WINDOW_Y)           { vals[i++] = ev->y; }
        if (mask & XCB_CONFIG_WINDOW_WIDTH)       { vals[i++] = ev->width; }
        if (mask & XCB_CONFIG_WINDOW_HEIGHT)      { vals[i++] = ev->height; }
        if (mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) { vals[i++] = ev->border_width; }
        if (mask & XCB_CONFIG_WINDOW_SIBLING)     { vals[i++] = ev->sibling; }
        if (mask & XCB_CONFIG_WINDOW_STACK_MODE)  { vals[i++] = ev->stack_mode; }
        xcb_configure_window(wm->conn, ev->window, mask, vals);
    }
    xcb_flush(wm->conn);
}

static void on_unmap_notify(Wm *wm, xcb_unmap_notify_event_t *ev)
{
    WmClient *c = wm_find_client_by_window(wm, ev->window);
    if (c && c->frame && ev->event == c->frame) {
        if (c->hidden) {
            return;
        }
        wm_remove_client(wm, c);
    }
}

static void on_destroy_notify(Wm *wm, xcb_destroy_notify_event_t *ev)
{
    wm_remove_dock(wm, ev->window);

    WmClient *c = wm_find_client_by_window(wm, ev->window);
    if (c) {
        wm_remove_client(wm, c);
    }
}

static void on_button_release(Wm *wm, xcb_button_release_event_t *ev);

static void on_motion_notify(Wm *wm, xcb_motion_notify_event_t *ev)
{
    WmClient *c = wm->drag_client;
    if (!c || wm->drag_mode == DRAG_NONE) {
        return;
    }

    int rx = ev->root_x;
    int ry = ev->root_y;
    int dx = rx - wm->drag_start_x;
    int dy = ry - wm->drag_start_y;

    if (wm->drag_mode == DRAG_MOVE) {
        c->x = wm->drag_orig_x + dx;
        c->y = wm->drag_orig_y + dy;
        uint32_t vals[] = { c->x, c->y };
        xcb_configure_window(wm->conn, c->frame,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                             vals);
        int zone = detect_snap_zone(wm, rx, ry);
        int mon  = monitor_at_point(wm, rx, ry);
        if (zone != SNAP_NONE) {
            if (wm->snap_pending != zone || wm->snap_monitor != mon) {
                snap_preview_show(wm, zone, mon);
                wm->snap_pending = zone;
                wm->snap_monitor = mon;
            }
        } else if (wm->snap_pending != SNAP_NONE) {
            snap_preview_hide(wm);
        }
    } else if (wm->drag_mode == DRAG_RESIZE) {
        int e = wm->resize_edge;
        int nx = wm->drag_orig_x;
        int ny = wm->drag_orig_y;
        int nw = wm->drag_orig_w;
        int nh = wm->drag_orig_h;

        if (e == GRIP_TOP || e == GRIP_TL || e == GRIP_TR) {
            nh = wm->drag_orig_h - dy;
            ny = wm->drag_orig_y + dy;
        }
        if (e == GRIP_BOTTOM || e == GRIP_BL || e == GRIP_BR) {
            nh = wm->drag_orig_h + dy;
        }
        if (e == GRIP_LEFT || e == GRIP_TL || e == GRIP_BL) {
            nw = wm->drag_orig_w - dx;
            nx = wm->drag_orig_x + dx;
        }
        if (e == GRIP_RIGHT || e == GRIP_TR || e == GRIP_BR) {
            nw = wm->drag_orig_w + dx;
        }

        int min_w = c->min_w > 0 ? c->min_w : 50;
        int min_h = c->min_h > 0 ? c->min_h : 50;

        if (c->inc_w > 1 || c->inc_h > 1) {
            int bw = c->base_w > 0 ? c->base_w : min_w;
            int bh = c->base_h > 0 ? c->base_h : min_h;
            nw = bw + ((nw - bw) / c->inc_w) * c->inc_w;
            nh = bh + ((nh - bh) / c->inc_h) * c->inc_h;
        }

        if (nw < min_w) { nw = min_w; nx = wm->drag_orig_x + wm->drag_orig_w - min_w; }
        if (nh < min_h) { nh = min_h; ny = wm->drag_orig_y + wm->drag_orig_h - min_h; }
        if (c->max_w > 0 && nw > c->max_w) { nw = c->max_w; }
        if (c->max_h > 0 && nh > c->max_h) { nh = c->max_h; }

        c->x = nx; c->y = ny;
        c->width = nw; c->height = nh;
        frame_configure(wm, c);
    }

    xcb_flush(wm->conn);
}

static void on_button_release(Wm *wm, xcb_button_release_event_t *ev)
{
    (void)ev;
    if (wm->drag_mode != DRAG_NONE) {
        WmClient *c = wm->drag_client;

        if (wm->drag_mode == DRAG_MOVE && wm->snap_pending != SNAP_NONE && c) {
            apply_snap(wm, c, wm->snap_pending, wm->snap_monitor);
        }
        snap_preview_hide(wm);

        xcb_ungrab_pointer(wm->conn, XCB_CURRENT_TIME);
        xcb_flush(wm->conn);
        wm->drag_mode   = DRAG_NONE;
        wm->drag_client = NULL;
    }
}

static void on_property_notify(Wm *wm, xcb_property_notify_event_t *ev)
{
    WmClient *c = wm_find_client_by_window(wm, ev->window);
    if (!c) {
        return;
    }

    if (ev->atom == wm->atom_wm_name || ev->atom == wm->atom_net_wm_name ||
        ev->atom == wm->atom_wm_icon_name ||
        ev->atom == wm->atom_net_wm_icon_name) {
        frame_update_title(wm, c);
    } else if (ev->atom == XCB_ATOM_WM_NORMAL_HINTS) {
        int was_fixed = c->fixed_size;
        frame_read_size_hints(wm, c);
        if (c->fixed_size && !was_fixed && c->grip[0]) {
            frame_destroy_grips(wm, c);
        } else if (!c->fixed_size && was_fixed && c->decorated && !c->grip[0]) {
            frame_create_grips(wm, c);
        }
    } else if (ev->atom == wm->atom_motif_wm_hints) {
        int dominated = wm_client_wants_decorations(wm, c->client);
        if (dominated != c->decorated) {
            c->decorated = dominated;
            frame_configure(wm, c);
        }
    } else if (ev->atom == wm->atom_net_wm_user_time) {
        xcb_get_property_reply_t *r = xcb_get_property_reply(wm->conn,
            xcb_get_property(wm->conn, 0, ev->window,
                             wm->atom_net_wm_user_time,
                             XCB_ATOM_CARDINAL, 0, 1), NULL);
        if (r && xcb_get_property_value_length(r) >= 4) {
            c->user_time = *(uint32_t *)xcb_get_property_value(r);
        }
        free(r);
    }
}

static void on_user_time_window_notify(Wm *wm, xcb_property_notify_event_t *ev)
{
    if (ev->atom != wm->atom_net_wm_user_time) {
        return;
    }
    for (WmClient *c = wm->clients; c; c = c->next) {
        if (c->user_time_window == ev->window) {
            xcb_get_property_reply_t *r = xcb_get_property_reply(wm->conn,
                xcb_get_property(wm->conn, 0, ev->window,
                                 wm->atom_net_wm_user_time,
                                 XCB_ATOM_CARDINAL, 0, 1), NULL);
            if (r && xcb_get_property_value_length(r) >= 4) {
                c->user_time = *(uint32_t *)xcb_get_property_value(r);
            }
            free(r);
            return;
        }
    }
}

static int on_client_message(Wm *wm, xcb_client_message_event_t *ev)
{
    xcb_ewmh_connection_t *ewmh = isde_ewmh_connection(wm->ewmh);

    if (ev->type == ewmh->_NET_ACTIVE_WINDOW) {
        uint32_t source = ev->data.data32[0];
        xcb_timestamp_t req_time = ev->data.data32[1];
        WmClient *c = wm_find_client_by_window(wm, ev->window);
        if (!c) { return 1; }

        if (source != 2 && wm->focused && wm->focused != c) {
            if (req_time == 0 || (wm->last_user_time != 0 &&
                (int32_t)(req_time - wm->last_user_time) < 0)) {
                return 1;
            }
        }

        xcb_map_window(wm->conn, c->frame);
        xcb_map_window(wm->conn, c->client);
        c->mapped = 1;
        wm_focus_client(wm, c, req_time ? req_time : XCB_CURRENT_TIME);
        return 1;
    } else if (ev->type == ewmh->_NET_CLOSE_WINDOW) {
        WmClient *c = wm_find_client_by_window(wm, ev->window);
        if (c) {
            wm_close_client(wm, c);
        }
        return 1;
    } else if (ev->type == ewmh->_NET_WM_STATE) {
        WmClient *c = wm_find_client_by_window(wm, ev->window);
        if (!c) { return 1; }

        uint32_t action = ev->data.data32[0];
        xcb_atom_t a1 = ev->data.data32[1];
        xcb_atom_t a2 = ev->data.data32[2];

        if (a1 == ewmh->_NET_WM_STATE_FULLSCREEN ||
            a2 == ewmh->_NET_WM_STATE_FULLSCREEN) {
            int want = (action == 1) || (action == 2 && !c->fullscreen);
            if (want != c->fullscreen) {
                wm_fullscreen_client(wm, c, want);
            }
        }
        if (a1 == ewmh->_NET_WM_STATE_MAXIMIZED_VERT ||
            a1 == ewmh->_NET_WM_STATE_MAXIMIZED_HORZ ||
            a2 == ewmh->_NET_WM_STATE_MAXIMIZED_VERT ||
            a2 == ewmh->_NET_WM_STATE_MAXIMIZED_HORZ) {
            int want = (action == 1) || (action == 2 && !c->maximized);
            if (want != c->maximized) {
                wm_maximize_client(wm, c);
            }
        }
        if (a1 == ewmh->_NET_WM_STATE_HIDDEN ||
            a2 == ewmh->_NET_WM_STATE_HIDDEN) {
            int want = (action == 1) || (action == 2 && !c->minimized);
            if (want && !c->minimized) {
                wm_minimize_client(wm, c);
            } else if (!want && c->minimized) {
                wm_restore_client(wm, c);
            }
        }
        if (a1 == ewmh->_NET_WM_STATE_ABOVE ||
            a2 == ewmh->_NET_WM_STATE_ABOVE) {
            int want = (action == 1) || (action == 2 && !c->above);
            wm_set_above(wm, c, want);
        }
        if (a1 == ewmh->_NET_WM_STATE_BELOW ||
            a2 == ewmh->_NET_WM_STATE_BELOW) {
            int want = (action == 1) || (action == 2 && !c->below);
            wm_set_below(wm, c, want);
        }
        if (a1 == ewmh->_NET_WM_STATE_MODAL ||
            a2 == ewmh->_NET_WM_STATE_MODAL) {
            c->modal = (action == 1) || (action == 2 && !c->modal);
            wm_update_net_wm_state(wm, c);
        }
        if (a1 == ewmh->_NET_WM_STATE_STICKY ||
            a2 == ewmh->_NET_WM_STATE_STICKY) {
            int want = (action == 1) || (action == 2 && !c->sticky);
            if (want && !c->sticky) {
                c->sticky = 1;
                wm_move_to_desktop(wm, c, 0xFFFFFFFF);
            } else if (!want && c->sticky) {
                c->sticky = 0;
                wm_move_to_desktop(wm, c, wm->current_desktop);
            }
            wm_update_net_wm_state(wm, c);
        }
        if (a1 == ewmh->_NET_WM_STATE_SKIP_TASKBAR ||
            a2 == ewmh->_NET_WM_STATE_SKIP_TASKBAR) {
            c->skip_taskbar = (action == 1) || (action == 2 && !c->skip_taskbar);
            wm_update_net_wm_state(wm, c);
        }
        if (a1 == ewmh->_NET_WM_STATE_SKIP_PAGER ||
            a2 == ewmh->_NET_WM_STATE_SKIP_PAGER) {
            c->skip_pager = (action == 1) || (action == 2 && !c->skip_pager);
            wm_update_net_wm_state(wm, c);
        }
        if (a1 == ewmh->_NET_WM_STATE_DEMANDS_ATTENTION ||
            a2 == ewmh->_NET_WM_STATE_DEMANDS_ATTENTION) {
            c->demands_attention = (action == 1) ||
                                   (action == 2 && !c->demands_attention);
            wm_update_net_wm_state(wm, c);
        }
        return 1;
    } else if (ev->type == ewmh->_NET_WM_MOVERESIZE) {
        WmClient *c = wm_find_client_by_window(wm, ev->window);
        if (!c) { return 1; }

        int root_x   = (int)ev->data.data32[0];
        int root_y   = (int)ev->data.data32[1];
        uint32_t dir = ev->data.data32[2];

        if (dir == XCB_EWMH_WM_MOVERESIZE_CANCEL) {
            if (wm->drag_mode != DRAG_NONE) {
                snap_preview_hide(wm);
                xcb_ungrab_pointer(wm->conn, XCB_CURRENT_TIME);
                wm->drag_mode = DRAG_NONE;
                wm->drag_client = NULL;
                xcb_flush(wm->conn);
            }
            return 1;
        }

        wm->drag_client  = c;
        wm->drag_start_x = root_x;
        wm->drag_start_y = root_y;
        wm->drag_orig_x  = c->x;
        wm->drag_orig_y  = c->y;
        wm->drag_orig_w  = c->width;
        wm->drag_orig_h  = c->height;

        if (dir == XCB_EWMH_WM_MOVERESIZE_MOVE ||
            dir == XCB_EWMH_WM_MOVERESIZE_MOVE_KEYBOARD) {
            wm->drag_mode = DRAG_MOVE;
        } else if (dir <= XCB_EWMH_WM_MOVERESIZE_SIZE_LEFT) {
            if (c->fixed_size) { return 1; }
            static const int dir_to_grip[] = {
                GRIP_TL, GRIP_TOP, GRIP_TR, GRIP_RIGHT,
                GRIP_BR, GRIP_BOTTOM, GRIP_BL, GRIP_LEFT
            };
            wm->drag_mode   = DRAG_RESIZE;
            wm->resize_edge = dir_to_grip[dir];
        } else {
            return 1;
        }

        xcb_grab_pointer(wm->conn, 1, wm->root,
                         XCB_EVENT_MASK_BUTTON_RELEASE |
                         XCB_EVENT_MASK_POINTER_MOTION,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                         XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
        xcb_flush(wm->conn);
        return 1;
    } else if (ev->type == wm->atom_wm_change_state) {
        WmClient *c = wm_find_client_by_window(wm, ev->window);
        if (c && ev->data.data32[0] == 3) {
            wm_minimize_client(wm, c);
        }
        return 1;
    } else if (ev->type == ewmh->_NET_CURRENT_DESKTOP) {
        uint32_t desk = ev->data.data32[0];
        wm_desktops_switch(wm, desk);
        return 1;
    } else if (ev->type == ewmh->_NET_WM_DESKTOP) {
        WmClient *c = wm_find_client_by_window(wm, ev->window);
        if (c) {
            uint32_t desk = ev->data.data32[0];
            if (desk < (uint32_t)wm->num_desktops || desk == 0xFFFFFFFF) {
                int was_visible = (c->desktop == wm->current_desktop ||
                                   c->desktop == 0xFFFFFFFF);
                c->desktop = desk;
                xcb_ewmh_set_wm_desktop(ewmh, c->client, desk);
                int is_visible = (desk == wm->current_desktop ||
                                  desk == 0xFFFFFFFF);
                if (was_visible && !is_visible) {
                    c->hidden = 1;
                    xcb_unmap_window(wm->conn, c->client);
                    if (c->frame && c->mapped) {
                        xcb_unmap_window(wm->conn, c->frame);
                        c->mapped = 0;
                    }
                    if (wm->focused == c) {
                        wm->focused = NULL;
                        wm_ewmh_update_active(wm);
                    }
                } else if (!was_visible && is_visible) {
                    c->hidden = 0;
                    xcb_map_window(wm->conn, c->client);
                    if (c->frame) {
                        xcb_map_window(wm->conn, c->frame);
                        c->mapped = 1;
                    }
                }
                xcb_flush(wm->conn);
            }
        }
        return 1;
    }

    if (ev->type == wm->atom_net_startup_info_begin ||
        ev->type == wm->atom_net_startup_info) {
        on_startup_info(wm, ev);
        return 1;
    }

    return 0;
}

/* ---------- title bar / button click handling ---------- */

/* A click landed on the frame window.  Title-bar buttons are hit-tested
 * by frame-relative coordinate; a click elsewhere in the title bar starts
 * a move drag. */
static void on_frame_button_press(Wm *wm, WmClient *c,
                                  xcb_button_press_event_t *ev)
{
    if (!c->decorated || ev->event_y >= wm->title_height) {
        return;
    }

    wm_focus_client(wm, c, ev->time);

    int btn = frame_button_at(wm, c, ev->event_x, ev->event_y);
    if (btn >= 0) {
        switch (btn) {
        case FRAME_BTN_MENU:
            win_menu_show(wm, c);
            break;
        case FRAME_BTN_MINIMIZE:
            wm_minimize_client(wm, c);
            break;
        case FRAME_BTN_MAXIMIZE:
            wm_maximize_client(wm, c);
            break;
        case FRAME_BTN_CLOSE:
            wm_close_client(wm, c);
            break;
        }
        return;
    }

    /* Not a button — start a move drag */
    wm->drag_mode    = DRAG_MOVE;
    wm->drag_client  = c;
    wm->drag_start_x = ev->root_x;
    wm->drag_start_y = ev->root_y;
    wm->drag_orig_x  = c->x;
    wm->drag_orig_y  = c->y;

    xcb_grab_pointer(wm->conn, 1, wm->root,
                     XCB_EVENT_MASK_BUTTON_RELEASE |
                     XCB_EVENT_MASK_POINTER_MOTION,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                     XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
    xcb_flush(wm->conn);
}

/* ---------- event loop ---------- */

static void dispatch_wm_event(Wm *wm, xcb_generic_event_t *ev)
{
    uint8_t type = ev->response_type & ~0x80;

    uint32_t cmd;
    if (isde_ipc_decode(wm->ipc, ev, &cmd, NULL, NULL, NULL, NULL)) {
        if (cmd == ISDE_CMD_QUIT) {
            wm->running = 0;
        }
        return;
    }

    /* Window menu handling */
    if (wm->win_menu && wm->menu_client) {
        if (type == XCB_BUTTON_PRESS) {
            xcb_button_press_event_t *bp = (xcb_button_press_event_t *)ev;
            if (bp->event == wm->win_menu) {
                win_menu_click(wm, bp->event_x, bp->event_y);
            } else {
                wm_dismiss_menu(wm);
            }
            return;
        }
        if (type == XCB_KEY_PRESS || type == XCB_KEY_RELEASE) {
            wm_dismiss_menu(wm);
            return;
        }
    }

    switch (type) {
    case XCB_MAP_REQUEST:
        on_map_request(wm, (xcb_map_request_event_t *)ev);
        break;
    case XCB_CONFIGURE_REQUEST:
        on_configure_request(wm, (xcb_configure_request_event_t *)ev);
        break;
    case XCB_UNMAP_NOTIFY: {
        xcb_unmap_notify_event_t *un = (xcb_unmap_notify_event_t *)ev;
#ifdef ISDE_COMPOSITOR
        if (wm->compositor && un->event == wm->root) {
            wm_compositor_set_mapped(wm->compositor, un->window, 0);
        }
#endif
        on_unmap_notify(wm, un);
        break;
    }
    case XCB_DESTROY_NOTIFY: {
        xcb_destroy_notify_event_t *dn = (xcb_destroy_notify_event_t *)ev;
#ifdef ISDE_COMPOSITOR
        if (wm->compositor) {
            wm_compositor_remove_window(wm->compositor, dn->window);
        }
#endif
        on_destroy_notify(wm, dn);
        break;
    }
#ifdef ISDE_COMPOSITOR
    case XCB_MAP_NOTIFY: {
        xcb_map_notify_event_t *mn = (xcb_map_notify_event_t *)ev;
        if (wm->compositor && mn->event == wm->root) {
            wm_compositor_add_window(wm->compositor, mn->window);
            wm_compositor_set_mapped(wm->compositor, mn->window, 1);
        }
        break;
    }
    case XCB_CONFIGURE_NOTIFY: {
        xcb_configure_notify_event_t *cn = (xcb_configure_notify_event_t *)ev;
        if (wm->compositor && cn->event == wm->root) {
            wm_compositor_window_configured(wm->compositor, cn->window,
                                            cn->x, cn->y,
                                            cn->width, cn->height,
                                            cn->border_width);
        }
        break;
    }
#endif
    case XCB_CLIENT_MESSAGE:
        on_client_message(wm, (xcb_client_message_event_t *)ev);
        break;
    case XCB_EXPOSE: {
        xcb_expose_event_t *xe = (xcb_expose_event_t *)ev;
        if (xe->count == 0) {
            WmClient *c = wm_find_client_by_frame(wm, xe->window);
            if (c) {
                frame_paint(wm, c);
            }
        }
        break;
    }
    case XCB_MOTION_NOTIFY:
        if (wm->drag_mode != DRAG_NONE) {
            on_motion_notify(wm, (xcb_motion_notify_event_t *)ev);
        }
        break;
    case XCB_BUTTON_RELEASE:
        if (wm->drag_mode != DRAG_NONE) {
            on_button_release(wm, (xcb_button_release_event_t *)ev);
        }
        break;
    case XCB_BUTTON_PRESS: {
        xcb_button_press_event_t *bp = (xcb_button_press_event_t *)ev;
        wm->last_user_time = bp->time;
        int edge;
        if (find_grip_client(wm, bp->event, &edge)) {
            on_grip_press(wm, bp);
        } else {
            WmClient *fc = wm_find_client_by_frame(wm, bp->event);
            if (fc) {
                on_frame_button_press(wm, fc, bp);
            } else {
                WmClient *c = wm_find_client_by_window(wm, bp->event);
                if (c) {
                    wm_focus_client(wm, c, bp->time);
                    xcb_allow_events(wm->conn, XCB_ALLOW_REPLAY_POINTER,
                                     bp->time);
                    xcb_flush(wm->conn);
                }
            }
        }
        break;
    }
    case XCB_PROPERTY_NOTIFY: {
        xcb_property_notify_event_t *pn = (xcb_property_notify_event_t *)ev;
        if (!wm_find_client_by_window(wm, pn->window)) {
            on_user_time_window_notify(wm, pn);
        } else {
            on_property_notify(wm, pn);
        }
        break;
    }
    case XCB_KEY_PRESS: {
        xcb_key_press_event_t *kp = (xcb_key_press_event_t *)ev;
        wm->last_user_time = kp->time;
        wm_keys_handle(wm, kp);
        break;
    }
    case XCB_KEY_RELEASE:
        wm_keys_handle_release(wm, (xcb_key_release_event_t *)ev);
        break;
    case XCB_SELECTION_CLEAR: {
        xcb_selection_clear_event_t *sc = (xcb_selection_clear_event_t *)ev;
        if (sc->selection == wm->atom_wm_sn) {
            fprintf(stderr, "isde-wm: replaced by another window manager\n");
            wm->running = 0;
        }
        break;
    }
    default:
        if (wm->randr_event_base &&
            (type == wm->randr_event_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY ||
             type == wm->randr_event_base + XCB_RANDR_NOTIFY)) {
            query_monitors(wm);
            rescue_orphaned_clients(wm);
        }
#ifdef ISDE_COMPOSITOR
        else if (wm->compositor &&
                 type == wm->compositor->damage_event_base +
                         XCB_DAMAGE_NOTIFY) {
            wm_compositor_handle_damage(wm->compositor,
                (xcb_damage_notify_event_t *)ev);
        }
#endif
        break;
    }
}

void wm_run(Wm *wm)
{
    int xcb_fd = xcb_get_file_descriptor(wm->conn);

    while (wm->running) {
        /* Fire expired timers */
        wm_timers_fire(wm);

        /* D-Bus dispatch */
        if (wm->dbus) {
            isde_dbus_dispatch(wm->dbus);
        }

        int timeout = wm_timer_next_timeout(wm);
        struct pollfd pfd = { .fd = xcb_fd, .events = POLLIN };
        poll(&pfd, 1, timeout);

        /* Drain all XCB events, coalescing consecutive motion events */
        xcb_generic_event_t *ev;
        xcb_generic_event_t *held_motion = NULL;
        while ((ev = xcb_poll_for_event(wm->conn))) {
            uint8_t type = ev->response_type & ~0x80;
            if (type == XCB_MOTION_NOTIFY && wm->drag_mode != DRAG_NONE) {
                free(held_motion);
                held_motion = ev;
                continue;
            }
            if (held_motion) {
                dispatch_wm_event(wm, held_motion);
                free(held_motion);
                held_motion = NULL;
            }
            dispatch_wm_event(wm, ev);
            free(ev);
        }
        if (held_motion) {
            dispatch_wm_event(wm, held_motion);
            free(held_motion);
        }

#ifdef ISDE_COMPOSITOR
        if (wm->compositor) {
            wm_compositor_paint(wm->compositor);
        }
#endif
    }
}

/* ---------- cleanup ---------- */

void wm_cleanup(Wm *wm)
{
    while (wm->clients) {
        wm_remove_client(wm, wm->clients);
    }

    while (wm->startup_seqs) {
        WmStartupSeq *s = wm->startup_seqs;
        wm->startup_seqs = s->next;
        if (s->timer_id >= 0) {
            wm_timer_remove(wm, s->timer_id);
        }
        free(s->id);
        free(s->wmclass);
        free(s);
    }

#ifdef ISDE_COMPOSITOR
    if (wm->compositor) {
        wm_compositor_destroy(wm->compositor);
        wm->compositor = NULL;
    }
#endif

    if (wm->wm_sn_owner) {
        xcb_destroy_window(wm->conn, wm->wm_sn_owner);
        wm->wm_sn_owner = 0;
    }

    /* Free cached icon surfaces */
    if (wm->icon_minimize) { cairo_surface_destroy(wm->icon_minimize); }
    if (wm->icon_maximize) { cairo_surface_destroy(wm->icon_maximize); }
    if (wm->icon_restore)  { cairo_surface_destroy(wm->icon_restore); }
    if (wm->icon_close)    { cairo_surface_destroy(wm->icon_close); }
    if (wm->icon_menu)     { cairo_surface_destroy(wm->icon_menu); }

    free(wm->docks);
    wm->docks = NULL;
    wm->ndocks = wm->cap_docks = 0;
    free(wm->monitors);
    wm->monitors = NULL;
    wm->nmonitors = 0;
    xcb_flush(wm->conn);

    if (wm->keysyms) {
        xcb_key_symbols_free(wm->keysyms);
    }
    isde_dbus_free(wm->dbus);
    isde_ipc_free(wm->ipc);
    isde_ewmh_free(wm->ewmh);

    xcb_disconnect(wm->conn);
}
