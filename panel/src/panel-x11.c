#define _POSIX_C_SOURCE 200809L
/*
 * panel-x11.c — panel's X11-specific operations.
 *
 * Holds the raw xcb / RandR / cursor / EWMH-window requests the panel makes,
 * moved out of panel.c. Function names are unchanged from panel.c.
 */
#include "panel-x11.h"
#include "../../platform/common/dbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_cursor.h>
#include <xcb/randr.h>

#include <ISW/ISWPlatform.h>
#include <ISW/ShellP.h>
#include "../../platform/X11/common/isde-monitor-xcb.h"

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

int panel_init_platform(Panel *p) {
    /* Query primary monitor geometry */
    query_primary_monitor(p);

    /* Clear override-redirect, set dock type + strut, select root events,
     * register the IPC handler, subscribe to RandR — all before mapping. */
    panel_setup_dock_window(p);
    //pager_init(p);

    return 0;
}

int panel_init_display_platform(Panel *p) {
    

    return 0;
}

int panel_init_display(Panel *p)
{
    PanelX11ServerContext *ctx = (PanelX11ServerContext *)malloc(sizeof(PanelX11ServerContext)); 
    const char *display = getenv("DISPLAY");
    int screen_num;
    ctx->conn = xcb_connect(display, &ctx->screen_num);
    if (xcb_connection_has_error(ctx->conn)) {
        fprintf(stderr, "isde-displayd: cannot connect to X\n");
        return 1;
    }

    ctx->ewmh = isde_ewmh_init(ctx->conn, ctx->screen_num);
    ctx->atom_net_wm_name         = intern(ctx->conn, "_NET_WM_NAME");
    ctx->atom_net_wm_visible_name = intern(ctx->conn, "_NET_WM_VISIBLE_NAME");
    ctx->atom_wm_name             = intern(ctx->conn, "WM_NAME");
    p->server_context = ctx;
    return 0;
}

void query_primary_monitor(Panel *p)
{
    PanelX11ServerContext *ctx = (PanelX11ServerContext *)p->server_context;
    xcb_screen_t *screen = (xcb_screen_t *)IswScreenNativeHandle((IswScreenOf(p->toplevel)));

    p->mon_x = 0;
    p->mon_y = 0;
    p->mon_w = screen->width_in_pixels;
    p->mon_h = screen->height_in_pixels;

    if (screen == NULL) return;

    const IsdeMonitorOps *ops = isde_monitor_xcb_probe(ctx->conn);
    if (ops) {
        IsdeMonitorXcbCtx mon_ctx = { ctx->conn, screen->root, screen };
        IsdeMonitor pm;
        if (ops->get_primary(&mon_ctx, &pm)) {
            p->mon_x = pm.x;
            p->mon_y = pm.y;
            p->mon_w = pm.width;
            p->mon_h = pm.height;
        }
    }
}

void panel_update_strut(Panel *p)
{
    Arg args[4];
    IswSetArg(args[0], IswNstrutBottom, p->phys_panel_h);
    IswSetArg(args[1], IswNstrutLeft, 0);
    IswSetArg(args[2], IswNstrutRight, 0);
    IswSetArg(args[3], IswNstrutTop, 0);
    IswSetValues(p->shell, args, 4);

    WMShellWidget wmshell = (WMShellWidget)p->shell;
    wmshell->wm.strut_partial.bottom_start_x = p->mon_x;
    wmshell->wm.strut_partial.bottom_end_x = p->mon_x + p->mon_w - 1;

    _IswPlatformSetStrutPartial(IswDisplayOf(p->shell),
        _IswPlatformWidgetWindow(IswDisplayOf(p->shell), p->shell),
        &wmshell->wm.strut_partial);
}

void panel_ungrab_popup(Panel *p)
{
    if (!p->active_popup) {
        return;
    }
    IswUngrabKeyboard(p->active_popup, ISW_CURRENT_TIME);
    IswUngrabPointer(p->active_popup, ISW_CURRENT_TIME);
}

void launch_cursor_init(Panel *p)
{
    //if (p->cursor_watch) {
    //    return;
    //}
    //xcb_cursor_context_t *ctx;
    //if (xcb_cursor_context_new(p->conn, p->screen, &ctx) < 0) {
    //    return;
    //}
    //p->cursor_watch = xcb_cursor_load_cursor(ctx, "watch");
    //p->cursor_default = xcb_cursor_load_cursor(ctx, "left_ptr");
    //xcb_cursor_context_free(ctx);
}

void set_panel_cursor(Panel *p, xcb_cursor_t cursor)
{
    PanelX11ServerContext *ctx = (PanelX11ServerContext *)p->server_context;
    if (!p->shell || !IswIsRealized(p->shell)) {
        return;
    }
    uint32_t vals[] = { cursor };
    //xcb_change_window_attributes(ctx->conn, IswWindow(p->shell), XCB_CW_CURSOR, vals);
    xcb_flush(ctx->conn);
}

char *get_wm_class(Panel *p, xcb_window_t win)
{
    PanelX11ServerContext *ctx = (PanelX11ServerContext *)p->server_context;
    char *instance = NULL, *class = NULL;
    if (isde_ewmh_get_wm_class(ctx->ewmh, win, &instance, &class)) {
        if (class && *class) {
            free(instance);
            return class;
        }
        /* Class empty but instance available */
        free(class);
        if (instance && *instance) {
            return instance;
        }
        free(instance);
    }

    /* Last resort: use the window title */
    char *title = get_window_title(p, win);
    return title;
}

char *get_window_title(Panel *p, xcb_window_t win)
{
    PanelX11ServerContext *ctx = (PanelX11ServerContext *)p->server_context;
    xcb_get_property_reply_t *reply = xcb_get_property_reply(
        ctx->conn,
        xcb_get_property(ctx->conn, 0, win, ctx->atom_net_wm_visible_name,
                         XCB_ATOM_ANY, 0, 256),
        NULL);
    if (reply && reply->value_len > 0) {
        char *title = strndup(xcb_get_property_value(reply),
                              reply->value_len);
        free(reply);
        return title;
    }
    free(reply);

    reply = xcb_get_property_reply(
        ctx->conn,
        xcb_get_property(ctx->conn, 0, win, ctx->atom_net_wm_name,
                         XCB_ATOM_ANY, 0, 256),
        NULL);
    if (reply && reply->value_len > 0) {
        char *title = strndup(xcb_get_property_value(reply),
                              reply->value_len);
        free(reply);
        return title;
    }
    free(reply);

    reply = xcb_get_property_reply(
        ctx->conn,
        xcb_get_property(ctx->conn, 0, win, ctx->atom_wm_name,
                         XCB_ATOM_ANY, 0, 256),
        NULL);
    if (reply && reply->value_len > 0) {
        char *title = strndup(xcb_get_property_value(reply),
                              reply->value_len);
        free(reply);
        return title;
    }
    free(reply);
    return strdup("(untitled)");
}

void focus_window(Panel *p, xcb_window_t win)
{
    PanelX11ServerContext *ctx = (PanelX11ServerContext *)p->server_context;
    /* Send _NET_ACTIVE_WINDOW — the WM should handle raising
     * and unmapping minimized windows */
    isde_ewmh_request_active_window(ctx->ewmh, win);
}

void panel_focus_window(Panel *p, TaskGroup *g, int idx)
{
    xcb_window_t *wins = (xcb_window_t *)g->windows;
    focus_window(p, wins[idx]);
}

void panel_setup_dock_window(Panel *p)
{
    PanelX11ServerContext *ctx = (PanelX11ServerContext *)p->server_context;
    xcb_screen_t *screen = (xcb_screen_t *)IswScreenNativeHandle((IswScreenOf(p->toplevel)));

    /* Watch root for:
     *   PROPERTY_CHANGE — client list updates
     *   STRUCTURE_NOTIFY — required for ClientMessages sent to root with
     *                      SUBSTRUCTURE_REDIRECT|STRUCTURE_NOTIFY mask
     *                      (isde_ipc_send uses that mask). */
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE |
                    XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(ctx->conn, screen->root,
                                 XCB_CW_EVENT_MASK, &mask);

    /* Subscribe to RandR screen change events */
    xcb_randr_select_input(ctx->conn, screen->root,
                           XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);

    xcb_flush(ctx->conn);
}

extern void free_menu_titles(TaskGroup *g);
void taskbar_update(Panel *p)
{
    PanelX11ServerContext *ctx = (PanelX11ServerContext *)p->server_context;

    /* Clear window lists from all groups */
    for (TaskGroup *g = p->groups; g; g = g->next) {
        g->nwindows = 0;
    }

    /* Get current client list from WM */
    xcb_window_t *wins = NULL;
    int nwins = isde_ewmh_get_client_list(ctx->ewmh, &wins);
    uint32_t cur_desk = isde_ewmh_get_current_desktop(ctx->ewmh);

    xcb_ewmh_connection_t *ec = isde_ewmh_connection(ctx->ewmh);

    for (int i = 0; i < nwins; i++) {
        uint32_t win_desk = isde_ewmh_get_wm_desktop(ctx->ewmh, wins[i]);
        if (win_desk != cur_desk && win_desk != 0xFFFFFFFF) {
            continue;
        }
        xcb_ewmh_get_atoms_reply_t wm_state;
        if (xcb_ewmh_get_wm_state_reply(ec,
                xcb_ewmh_get_wm_state(ec, wins[i]),
                &wm_state, NULL)) {
            int skip = 0;
            for (uint32_t s = 0; s < wm_state.atoms_len; s++) {
                if (wm_state.atoms[s] == ec->_NET_WM_STATE_SKIP_TASKBAR) {
                    skip = 1;
                    break;
                }
            }
            xcb_ewmh_get_atoms_reply_wipe(&wm_state);
            if (skip) {
                continue;
            }
        }
        char *cls = get_wm_class(p, wins[i]);
        TaskGroup *g = taskbar_find_group(p, cls);
        if (!g) {
            g = taskbar_add_group(p, cls);
        }
        group_add_window(g, wins[i]);
        free(cls);
    }
    free(wins);

    /* Remove non-pinned groups with 0 windows */
    TaskGroup **pp = &p->groups;
    while (*pp) {
        TaskGroup *g = *pp;
        if (g->nwindows == 0 && !g->pinned) {
            *pp = g->next;
            free_menu_titles(g);
            if (g->menu) {
                IswDestroyWidget(g->menu);
            }
            if (g->ctx_menu) {
                IswDestroyWidget(g->ctx_menu);
            }
            if (g->button) {
                IswDestroyWidget(g->button);
            }
            free(g->wm_class);
            free(g->display_name);
            free(g->desktop_exec);
            free(g->desktop_icon);
            free(g->icon_path);
            free(g->windows);
            free(g);
        } else {
            pp = &g->next;
        }
    }
}

/* ---------- reconfigure on monitor change ---------- */

void panel_reconfigure(Panel *p)
{
    int16_t  old_x = p->mon_x;
    uint16_t old_w = p->mon_w, old_h = p->mon_h;

    query_primary_monitor(p);

    if (p->mon_x == old_x && p->mon_w == old_w && p->mon_h == old_h) {
        return;
    }

    /* All IswConfigureWidget values must be logical — ISW scales internally */
    double sf = ISWScaleFactor(p->toplevel);
    int log_x = (int)(p->mon_x / sf + 0.5);
    int log_w = (int)(p->mon_w / sf + 0.5);
    int log_y = (int)((p->mon_y + p->mon_h) / sf + 0.5) - PANEL_HEIGHT;

    IswConfigureWidget(p->shell, log_x, log_y, log_w, PANEL_HEIGHT, 0);

    /* Update strut */
    panel_update_strut(p);

    fprintf(stderr, "isde-panel: reconfigured for %dx%d+%d+%d\n",
            p->mon_w, p->mon_h, p->mon_x, p->mon_y);
}

void group_add_window(TaskGroup *g, xcb_window_t win)
{
    xcb_window_t *wins = (xcb_window_t *)g->windows;
    for (int i = 0; i < g->nwindows; i++) {
        if (wins[i] == win) {
            return;
        }
    }

    if (g->nwindows >= g->cap_windows) {
        g->cap_windows *= 2;
        g->windows = realloc(g->windows,
                             g->cap_windows * sizeof(xcb_window_t));
        wins = (xcb_window_t *)g->windows;
    }
    wins[g->nwindows++] = win;
}

void close_window(void *server_ctx, TaskGroup *g, int idx) {
    PanelX11ServerContext *ctx = (PanelX11ServerContext *)server_ctx;
    xcb_window_t *wins = (xcb_window_t *) g->windows;
    isde_ewmh_request_close_window(ctx->ewmh, wins[idx]);
}

char *panel_get_window_title(Panel *p, TaskGroup *g, int idx)
{
    xcb_window_t *wins = (xcb_window_t *)g->windows;
    return get_window_title(p, wins[idx]);
}

Pixel panel_color_pixel(Panel *p, unsigned int rgb)
{
    PanelX11ServerContext *ctx = (PanelX11ServerContext *)p->server_context;
    xcb_screen_t *screen = (xcb_screen_t *)IswScreenNativeHandle((IswScreenOf(p->toplevel)));
    xcb_alloc_color_reply_t *reply = xcb_alloc_color_reply(
        ctx->conn,
        xcb_alloc_color(ctx->conn, screen->default_colormap,
                        ((rgb >> 16) & 0xFF) * 257,
                        ((rgb >> 8)  & 0xFF) * 257,
                        ( rgb        & 0xFF) * 257),
        NULL);
    if (!reply) {
        return screen->white_pixel;
    }
    Pixel px = reply->pixel;
    free(reply);
    return px;
}