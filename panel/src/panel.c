#define _POSIX_C_SOURCE 200809L
/*
 * panel.c — panel initialization, dock window, event integration
 */
#include "panel.h"
#include <X11/ShellP.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb_aux.h>
#include <xcb/randr.h>

static xcb_atom_t intern(xcb_connection_t *c, const char *name)
{
    xcb_intern_atom_cookie_t ck = xcb_intern_atom(c, 0, strlen(name), name);
    xcb_intern_atom_reply_t *r  = xcb_intern_atom_reply(c, ck, NULL);
    if (!r) return XCB_ATOM_NONE;
    xcb_atom_t a = r->atom;
    free(r);
    return a;
}

static void load_pinned(Panel *p)
{
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (!cfg) return;

    IsdeConfigTable *root = isde_config_root(cfg);
    IsdeConfigTable *panel_cfg = isde_config_table(root, "panel");
    if (panel_cfg) {
        char **classes = NULL;
        int n = isde_config_string_array(panel_cfg, "pinned", &classes);
        if (n > 0 && classes) {
            p->pinned_classes = classes;
            p->npinned = n;
        }
    }
    isde_config_free(cfg);
}

static void load_desktop_entries(Panel *p)
{
    /* Scan XDG_DATA_DIRS/applications/ for .desktop files */
    const char *data_dirs = isde_xdg_data_dirs();
    const char *dp = data_dirs;

    while (dp && *dp) {
        const char *colon = strchr(dp, ':');
        size_t dlen = colon ? (size_t)(colon - dp) : strlen(dp);
        if (dlen > 0) {
            char path[512];
            snprintf(path, sizeof(path), "%.*s/applications",
                     (int)dlen, dp);
            int count = 0;
            IsdeDesktopEntry **entries =
                isde_desktop_scan_dir(path, &count);
            if (entries && count > 0) {
                p->desktop_entries = realloc(p->desktop_entries,
                    (p->ndesktop + count) * sizeof(IsdeDesktopEntry *));
                for (int i = 0; i < count; i++)
                    p->desktop_entries[p->ndesktop++] = entries[i];
                free(entries);
            }
        }
        dp = colon ? colon + 1 : NULL;
    }

    /* Also scan XDG_DATA_HOME/applications/ */
    char path[512];
    snprintf(path, sizeof(path), "%s/applications",
             isde_xdg_data_home());
    int count = 0;
    IsdeDesktopEntry **entries = isde_desktop_scan_dir(path, &count);
    if (entries && count > 0) {
        p->desktop_entries = realloc(p->desktop_entries,
            (p->ndesktop + count) * sizeof(IsdeDesktopEntry *));
        for (int i = 0; i < count; i++)
            p->desktop_entries[p->ndesktop++] = entries[i];
        free(entries);
    }
}

/* Find a .desktop entry whose WM_CLASS (StartupWMClass) or executable
 * name matches the given class */
static IsdeDesktopEntry *find_desktop_for_class(Panel *p,
                                                 const char *wm_class)
{
    if (!wm_class) return NULL;
    /* TODO: match StartupWMClass field when we add it to the parser.
     * For now, try matching the lowercase exec basename against
     * the lowercase class name. */
    char class_lower[128];
    int i;
    for (i = 0; wm_class[i] && i < 126; i++)
        class_lower[i] = (wm_class[i] >= 'A' && wm_class[i] <= 'Z')
                        ? wm_class[i] + 32 : wm_class[i];
    class_lower[i] = '\0';

    for (int j = 0; j < p->ndesktop; j++) {
        const char *exec = isde_desktop_exec(p->desktop_entries[j]);
        if (!exec) continue;
        /* Get basename of exec */
        const char *base = strrchr(exec, '/');
        base = base ? base + 1 : exec;
        /* Strip any arguments */
        char name[128];
        int k;
        for (k = 0; base[k] && base[k] != ' ' && k < 126; k++)
            name[k] = (base[k] >= 'A' && base[k] <= 'Z')
                     ? base[k] + 32 : base[k];
        name[k] = '\0';

        if (strcmp(name, class_lower) == 0)
            return p->desktop_entries[j];
    }
    return NULL;
}

/* Query primary monitor geometry via RandR.
 * Falls back to full screen if RandR is unavailable or no primary is set. */
static void query_primary_monitor(Panel *p)
{
    /* Default to full screen */
    p->mon_x = 0;
    p->mon_y = 0;
    p->mon_w = p->screen->width_in_pixels;
    p->mon_h = p->screen->height_in_pixels;

    xcb_randr_get_output_primary_reply_t *primary =
        xcb_randr_get_output_primary_reply(p->conn,
            xcb_randr_get_output_primary(p->conn, p->root), NULL);
    if (!primary)
        return;

    xcb_randr_output_t pout = primary->output;
    free(primary);

    if (pout == XCB_NONE)
        return;

    xcb_randr_get_output_info_reply_t *oinfo =
        xcb_randr_get_output_info_reply(p->conn,
            xcb_randr_get_output_info(p->conn, pout, XCB_CURRENT_TIME),
            NULL);
    if (!oinfo)
        return;

    xcb_randr_crtc_t crtc = oinfo->crtc;
    free(oinfo);

    if (crtc == XCB_NONE)
        return;

    xcb_randr_get_crtc_info_reply_t *cinfo =
        xcb_randr_get_crtc_info_reply(p->conn,
            xcb_randr_get_crtc_info(p->conn, crtc, XCB_CURRENT_TIME),
            NULL);
    if (!cinfo)
        return;

    p->mon_x = cinfo->x;
    p->mon_y = cinfo->y;
    p->mon_w = cinfo->width;
    p->mon_h = cinfo->height;
    free(cinfo);
}

static void panel_reconfigure(Panel *p);

/* Forward declarations */
static void on_panel_settings_changed(const char *, const char *, void *);
static void panel_dbus_input_cb(XtPointer, int *, XtInputId *);

int panel_init(Panel *p, int *argc, char **argv)
{
    memset(p, 0, sizeof(*p));

    char **fallbacks = isde_theme_build_resources();
    p->toplevel = XtAppInitialize(&p->app, "ISDE-Panel",
                                  NULL, 0, argc, argv,
                                  fallbacks, NULL, 0);

    p->conn = XtDisplay(p->toplevel);
    if (xcb_connection_has_error(p->conn))
        return -1;

    p->screen = XtScreen(p->toplevel);
    p->root = p->screen->root;

    /* Find screen number */
    p->screen_num = 0;
    xcb_screen_iterator_t si = xcb_setup_roots_iterator(
        xcb_get_setup(p->conn));
    for (int i = 0; si.rem; xcb_screen_next(&si), i++) {
        if (si.data->root == p->root) {
            p->screen_num = i;
            break;
        }
    }

    p->ewmh = isde_ewmh_init(p->conn, p->screen_num);
    p->ipc  = isde_ipc_init(p->conn, p->screen_num);

    p->atom_net_wm_name = intern(p->conn, "_NET_WM_NAME");
    p->atom_wm_name     = intern(p->conn, "WM_NAME");

    /* Load config and desktop entries */
    load_pinned(p);
    load_desktop_entries(p);

    /* Query primary monitor geometry */
    query_primary_monitor(p);

    /* Create panel shell — override-redirect dock at bottom of primary */
    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNx, p->mon_x);                    n++;
    XtSetArg(args[n], XtNy, p->mon_y + p->mon_h
                              - PANEL_HEIGHT);             n++;
    XtSetArg(args[n], XtNwidth, p->mon_w);                  n++;
    XtSetArg(args[n], XtNheight, PANEL_HEIGHT);            n++;
    XtSetArg(args[n], XtNoverrideRedirect, True);          n++;
    XtSetArg(args[n], XtNborderWidth, 0);                  n++;
    p->shell = XtCreatePopupShell("panel", overrideShellWidgetClass,
                                  p->toplevel, args, n);

    /* Form layout: start button | taskbar box | clock */
    n = 0;
    XtSetArg(args[n], XtNdefaultDistance, 0); n++;
    XtSetArg(args[n], XtNborderWidth, 0);     n++;
    p->form = XtCreateManagedWidget("panelForm", formWidgetClass,
                                    p->shell, args, n);

    /* Initialize applets — they create widgets inside p->form */
    startmenu_init(p);

    /* Taskbar box in the middle */
    n = 0;
    XtSetArg(args[n], XtNorientation, XtorientHorizontal); n++;
    XtSetArg(args[n], XtNborderWidth, 0);                   n++;
    XtSetArg(args[n], XtNhSpace, 0);                        n++;
    XtSetArg(args[n], XtNvSpace, 0);                        n++;
    XtSetArg(args[n], XtNfromHoriz, p->start_btn);          n++;
    XtSetArg(args[n], XtNtop, XtChainTop);                  n++;
    XtSetArg(args[n], XtNbottom, XtChainBottom);            n++;
    XtSetArg(args[n], XtNleft, XtChainLeft);                n++;
    XtSetArg(args[n], XtNright, XtChainRight);              n++;
    XtSetArg(args[n], XtNheight, PANEL_HEIGHT);             n++;
    p->box = XtCreateManagedWidget("panelBox", boxWidgetClass,
                                   p->form, args, n);

    taskbar_init(p);
    clock_init(p);

    XtRealizeWidget(p->shell);
    XtPopup(p->shell, XtGrabNone);

    /* Set _NET_WM_WINDOW_TYPE_DOCK and strut */
    xcb_ewmh_connection_t *ewmh = isde_ewmh_connection(p->ewmh);
    xcb_window_t panel_win = XtWindow(p->shell);

    xcb_atom_t dock_type = ewmh->_NET_WM_WINDOW_TYPE_DOCK;
    xcb_ewmh_set_wm_window_type(ewmh, panel_win, 1, &dock_type);

    /* Reserve space at bottom of screen */
    xcb_ewmh_wm_strut_partial_t strut;
    memset(&strut, 0, sizeof(strut));
    strut.bottom = PANEL_HEIGHT;
    strut.bottom_start_x = p->mon_x;
    strut.bottom_end_x = p->mon_x + p->mon_w - 1;
    xcb_ewmh_set_wm_strut_partial(ewmh, panel_win, strut);

    /* Watch root for property changes (client list updates) */
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(p->conn, p->root,
                                 XCB_CW_EVENT_MASK, &mask);

    /* Subscribe to RandR screen change events */
    xcb_randr_select_input(p->conn, p->root,
                           XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);

    xcb_flush(p->conn);

    /* D-Bus settings notifications */
    p->dbus = isde_dbus_init();
    if (p->dbus) {
        isde_dbus_settings_subscribe(p->dbus, on_panel_settings_changed, p);
        int dbus_fd = isde_dbus_get_fd(p->dbus);
        if (dbus_fd >= 0)
            XtAppAddInput(p->app, dbus_fd,
                          (XtPointer)XtInputReadMask,
                          panel_dbus_input_cb, p->dbus);
    }

    p->running = 1;
    return 0;
}

/* ---------- reconfigure on monitor change ---------- */

static void panel_reconfigure(Panel *p)
{
    int16_t  old_x = p->mon_x;
    uint16_t old_w = p->mon_w, old_h = p->mon_h;

    query_primary_monitor(p);

    if (p->mon_x == old_x && p->mon_w == old_w && p->mon_h == old_h)
        return;

    int panel_y = p->mon_y + p->mon_h - PANEL_HEIGHT;

    /* Resize panel shell */
    XtConfigureWidget(p->shell, p->mon_x, panel_y,
                      p->mon_w, PANEL_HEIGHT, 0);

    /* Reposition clock */
    int clock_x = p->mon_w - PANEL_CLOCK_WIDTH - 2;
    int half = PANEL_HEIGHT / 2;
    XtConfigureWidget(p->clock_time, clock_x, 0,
                      PANEL_CLOCK_WIDTH, half, 0);
    XtConfigureWidget(p->clock_date, clock_x, half,
                      PANEL_CLOCK_WIDTH, half, 0);

    /* Update strut */
    xcb_ewmh_connection_t *ewmh = isde_ewmh_connection(p->ewmh);
    xcb_ewmh_wm_strut_partial_t strut;
    memset(&strut, 0, sizeof(strut));
    strut.bottom = PANEL_HEIGHT;
    strut.bottom_start_x = p->mon_x;
    strut.bottom_end_x = p->mon_x + p->mon_w - 1;
    xcb_ewmh_set_wm_strut_partial(ewmh, XtWindow(p->shell), strut);
    xcb_flush(p->conn);

    fprintf(stderr, "isde-panel: reconfigured for %dx%d+%d+%d\n",
            p->mon_w, p->mon_h, p->mon_x, p->mon_y);
}

/* ---------- D-Bus settings reload ---------- */

static Pixel panel_color_pixel(Panel *p, unsigned int rgb)
{
    xcb_alloc_color_reply_t *reply = xcb_alloc_color_reply(
        p->conn,
        xcb_alloc_color(p->conn, p->screen->default_colormap,
                        ((rgb >> 16) & 0xFF) * 257,
                        ((rgb >> 8)  & 0xFF) * 257,
                        ( rgb        & 0xFF) * 257),
        NULL);
    if (!reply) return p->screen->white_pixel;
    Pixel px = reply->pixel;
    free(reply);
    return px;
}

/* Theme colors are applied via Xresources (isde_theme_build_resources).
 * No manual XtSetValues needed — all widget names match resource specs. */


static void on_panel_settings_changed(const char *section, const char *key,
                                      void *user_data)
{
    (void)key;
    Panel *p = (Panel *)user_data;
    if (strcmp(section, "panel.clock") == 0 ||
        strcmp(section, "panel") == 0 ||
        strcmp(section, "appearance") == 0 ||
        strcmp(section, "*") == 0) {
        p->running = 0;
        p->restart = 1;
    }
}

static void panel_dbus_input_cb(XtPointer client_data, int *fd, XtInputId *id)
{
    (void)fd;
    (void)id;
    IsdeDBus *bus = (IsdeDBus *)client_data;
    isde_dbus_dispatch(bus);
}

/* Periodic poll for client list changes — called every 50ms.
 * This catches _NET_CLIENT_LIST updates that Xt's event dispatch
 * would otherwise drop (PropertyNotify on root, no widget). */
static xcb_window_t last_active = XCB_WINDOW_NONE;

static void poll_clients(XtPointer client_data, XtIntervalId *id)
{
    (void)id;
    Panel *p = (Panel *)client_data;

    /* Check for screen changes (RandR) */
    panel_reconfigure(p);

    taskbar_update(p);
    taskbar_highlight_active(p);

    /* Dismiss popups when focus moves to a managed window */
    if (p->active_popup) {
        xcb_window_t active = isde_ewmh_get_active_window(p->ewmh);
        if (active != XCB_WINDOW_NONE && active != last_active) {
            panel_dismiss_popup(p);
        }
        last_active = active;
    } else {
        last_active = isde_ewmh_get_active_window(p->ewmh);
    }

    XtAppAddTimeOut(p->app, 50, poll_clients, p);
}

void panel_show_popup(Panel *p, Widget popup)
{
    panel_dismiss_popup(p);
    p->active_popup = popup;
}

void panel_dismiss_popup(Panel *p)
{
    if (!p->active_popup) return;

    /* Reset start button to inactive state */
    if (p->active_popup == p->start_shell) {
        const IsdeColorScheme *s = isde_theme_current();
        if (s) {
            Pixel fg = panel_color_pixel(p, s->taskbar_button.fg);
            Pixel bg = panel_color_pixel(p, s->taskbar_button.bg);
            Arg ia[2];
            XtSetArg(ia[0], XtNforeground, fg);
            XtSetArg(ia[1], XtNbackground, bg);
            XtSetValues(p->start_btn, ia, 2);
        }
    }

    ShellWidget sw = (ShellWidget)p->active_popup;
    if (sw->shell.popped_up)
        XtPopdown(p->active_popup);

    p->active_popup = NULL;
}

void panel_run(Panel *p)
{
    /* Initial taskbar population */
    taskbar_update(p);

    /* Start periodic client list poll */
    XtAppAddTimeOut(p->app, 50, poll_clients, p);

    while (p->running) {
        XtAppProcessEvent(p->app, XtIMAll);
    }
}

void panel_cleanup(Panel *p)
{
    clock_cleanup(p);
    taskbar_cleanup(p);
    startmenu_cleanup(p);

    for (int i = 0; i < p->ndesktop; i++)
        isde_desktop_free(p->desktop_entries[i]);
    free(p->desktop_entries);

    for (int i = 0; i < p->npinned; i++)
        free(p->pinned_classes[i]);
    free(p->pinned_classes);

    isde_dbus_free(p->dbus);
    isde_ipc_free(p->ipc);
    isde_ewmh_free(p->ewmh);
    XtDestroyApplicationContext(p->app);
}
