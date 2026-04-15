#define _POSIX_C_SOURCE 200809L
/*
 * tray.c — system tray (freedesktop System Tray Protocol)
 *
 * The panel claims the _NET_SYSTEM_TRAY_S<n> selection to become
 * the system tray manager. Tray clients send a ClientMessage to
 * request docking; we reparent their window into a Box widget.
 */
#include "panel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* System tray opcodes */
#define SYSTEM_TRAY_REQUEST_DOCK    0
#define SYSTEM_TRAY_BEGIN_MESSAGE   1
#define SYSTEM_TRAY_CANCEL_MESSAGE  2

/* XEMBED messages */
#define XEMBED_EMBEDDED_NOTIFY      0

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

static void send_xembed_notify(Panel *p, xcb_window_t icon)
{
    xcb_client_message_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = icon;
    ev.type = p->atom_xembed;
    ev.format = 32;
    ev.data.data32[0] = XCB_CURRENT_TIME;
    ev.data.data32[1] = XEMBED_EMBEDDED_NOTIFY;
    ev.data.data32[2] = 0; /* version */
    ev.data.data32[3] = IswWindow(p->tray_box);
    xcb_send_event(p->conn, 0, icon,
                   XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
}

static void tray_dock_icon(Panel *p, xcb_window_t icon)
{
    /* Check for duplicates */
    for (int i = 0; i < p->ntray; i++) {
        if (p->tray_icons[i] == icon) {
            return;
        }
    }

    if (p->ntray >= p->cap_tray) {
        p->cap_tray = p->cap_tray ? p->cap_tray * 2 : 8;
        p->tray_icons = realloc(p->tray_icons,
                                p->cap_tray * sizeof(xcb_window_t));
    }
    p->tray_icons[p->ntray++] = icon;

    int icon_size = PANEL_HEIGHT - 4;
    int stride = icon_size + 2;
    int tray_w = p->ntray * stride + 2;

    /* Tell the FlexBox our new size */
    Arg a[20];
    Cardinal na = 0;
    IswSetArg(a[na], IswNflexBasis, tray_w); na++;
    IswSetValues(p->tray_box, a, na);

    /* Get physical dimensions after relayout */
    double sf = ISWScaleFactor(p->toplevel);
    int phys_icon = (int)(icon_size * sf + 0.5);
    int phys_stride = (int)(stride * sf + 0.5);

    /* Reparent new icon and position all icons left-to-right */
    xcb_reparent_window(p->conn, icon, IswWindow(p->tray_box),
                        (p->ntray - 1) * phys_stride, 2);
    for (int i = 0; i < p->ntray - 1; i++) {
        uint32_t xy[] = { i * phys_stride, 2 };
        xcb_configure_window(p->conn, p->tray_icons[i],
                             XCB_CONFIG_WINDOW_X |
                             XCB_CONFIG_WINDOW_Y, xy);
    }

    /* Resize the icon to fit */
    uint32_t vals[] = { phys_icon, phys_icon };
    xcb_configure_window(p->conn, icon,
                         XCB_CONFIG_WINDOW_WIDTH |
                         XCB_CONFIG_WINDOW_HEIGHT, vals);

    /* Register icon window into Xt dispatch so events reach our handler */
    IswRegisterDrawable(p->conn, icon, p->tray_box);
    uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(p->conn, icon, XCB_CW_EVENT_MASK, &mask);

    /* Map the icon */
    xcb_map_window(p->conn, icon);

    /* Send XEMBED_EMBEDDED_NOTIFY */
    send_xembed_notify(p, icon);

    xcb_flush(p->conn);

    fprintf(stderr, "isde-panel: tray: docked icon 0x%x (%d total)\n",
            icon, p->ntray);
}

static void tray_undock_icon(Panel *p, xcb_window_t icon)
{
    int found = -1;
    for (int i = 0; i < p->ntray; i++) {
        if (p->tray_icons[i] == icon) { found = i; break; }
    }
    if (found < 0) {
        return;
    }

    IswUnregisterDrawable(p->conn, icon);

    /* Remove from array */
    p->ntray--;
    for (int i = found; i < p->ntray; i++) {
        p->tray_icons[i] = p->tray_icons[i + 1];
    }

    int icon_size = PANEL_HEIGHT - 4;
    int stride = icon_size + 2;
    int tray_w = p->ntray > 0 ? p->ntray * stride + 2 : 2;

    Arg a[20];
    Cardinal na = 0;
    IswSetArg(a[na], IswNflexBasis, tray_w); na++;
    IswSetValues(p->tray_box, a, na);

    double sf = ISWScaleFactor(p->toplevel);
    int phys_stride = (int)(stride * sf + 0.5);

    for (int i = 0; i < p->ntray; i++) {
        uint32_t xy[] = { i * phys_stride, 2 };
        xcb_configure_window(p->conn, p->tray_icons[i],
                             XCB_CONFIG_WINDOW_X |
                             XCB_CONFIG_WINDOW_Y, xy);
    }

    xcb_flush(p->conn);

    fprintf(stderr, "isde-panel: tray: undocked icon 0x%x (%d remaining)\n",
            icon, p->ntray);
}

void tray_init_widgets(Panel *p)
{
    /* Intern atoms */
    char sel_name[32];
    snprintf(sel_name, sizeof(sel_name), "_NET_SYSTEM_TRAY_S%d",
             p->screen_num);
    p->atom_tray_sel    = intern(p->conn, sel_name);
    p->atom_tray_opcode = intern(p->conn, "_NET_SYSTEM_TRAY_OPCODE");
    p->atom_xembed      = intern(p->conn, "_XEMBED");
    p->atom_xembed_info = intern(p->conn, "_XEMBED_INFO");

    /* Create tray box — FlexBox child, fixed size, grows via IswSetValues */
    Arg args[20];
    Cardinal n = 0;
    IswSetArg(args[n], IswNorientation, XtorientHorizontal); n++;
    IswSetArg(args[n], IswNborderWidth, 0);                   n++;
    IswSetArg(args[n], IswNhSpace, 0);                        n++;
    IswSetArg(args[n], IswNvSpace, 0);                        n++;
    IswSetArg(args[n], IswNheight, PANEL_HEIGHT);             n++;
    IswSetArg(args[n], IswNflexBasis, 2);                     n++;
    p->tray_box = IswCreateManagedWidget("trayBox", boxWidgetClass,
                                        p->form, args, n);
}

static void tray_claim_selection(Panel *p);

/* Xt event handler on the shell — catches ClientMessage (dock requests)
 * and SelectionClear (loss of tray ownership).  Both are nonmaskable. */
static void shell_event_handler(Widget w, IswPointer closure,
                                xcb_generic_event_t *ev, Boolean *cont)
{
    (void)w;
    Panel *p = (Panel *)closure;
    uint8_t type = ev->response_type & ~0x80;

    if (type == XCB_CLIENT_MESSAGE) {
        xcb_client_message_event_t *cm = (xcb_client_message_event_t *)ev;
        if (cm->type == p->atom_tray_opcode &&
            cm->data.data32[1] == SYSTEM_TRAY_REQUEST_DOCK) {
            xcb_window_t icon = cm->data.data32[2];
            tray_dock_icon(p, icon);
        }
    } else if (type == XCB_SELECTION_CLEAR) {
        xcb_selection_clear_event_t *sc = (xcb_selection_clear_event_t *)ev;
        if (sc->selection == p->atom_tray_sel) {
            fprintf(stderr, "isde-panel: tray: lost selection, reclaiming\n");
            tray_claim_selection(p);
        }
    }
    *cont = True;
}

/* Xt event handler on the tray box — catches Destroy/Reparent from
 * tray icon children via SubstructureNotify. */
static void traybox_event_handler(Widget w, IswPointer closure,
                                  xcb_generic_event_t *ev, Boolean *cont)
{
    (void)w;
    Panel *p = (Panel *)closure;
    uint8_t type = ev->response_type & ~0x80;

    if (type == XCB_DESTROY_NOTIFY) {
        xcb_destroy_notify_event_t *dn = (xcb_destroy_notify_event_t *)ev;
        tray_undock_icon(p, dn->window);
    } else if (type == XCB_UNMAP_NOTIFY) {
        xcb_unmap_notify_event_t *un = (xcb_unmap_notify_event_t *)ev;
        tray_undock_icon(p, un->window);
    } else if (type == XCB_REPARENT_NOTIFY) {
        /* Client withdrew from tray — reparented away from our box */
        xcb_reparent_notify_event_t *rn = (xcb_reparent_notify_event_t *)ev;
        if (rn->parent != IswWindow(p->tray_box)) {
            tray_undock_icon(p, rn->window);
        }
    }
    *cont = True;
}

/* Claim (or reclaim) the tray selection and announce to clients */
/* Expand 8-bit channel to 16-bit (0xFF -> 0xFFFF) */
static uint32_t expand8to16(unsigned int c8)
{
    return (c8 << 8) | c8;
}

static void tray_set_colors(Panel *p)
{
    const IsdeColorScheme *s = isde_theme_current();
    if (!s)
        return;

    xcb_atom_t atom = intern(p->conn, "_NET_SYSTEM_TRAY_COLORS");
    if (atom == XCB_ATOM_NONE)
        return;

    /* 12 CARDINALs: fg R/G/B, error R/G/B, warning R/G/B, success R/G/B */
    uint32_t colors[12];
    /* foreground */
    colors[0]  = expand8to16((s->taskbar.fg >> 16) & 0xFF);
    colors[1]  = expand8to16((s->taskbar.fg >>  8) & 0xFF);
    colors[2]  = expand8to16( s->taskbar.fg        & 0xFF);
    /* error */
    colors[3]  = expand8to16((s->error >> 16) & 0xFF);
    colors[4]  = expand8to16((s->error >>  8) & 0xFF);
    colors[5]  = expand8to16( s->error        & 0xFF);
    /* warning */
    colors[6]  = expand8to16((s->warning >> 16) & 0xFF);
    colors[7]  = expand8to16((s->warning >>  8) & 0xFF);
    colors[8]  = expand8to16( s->warning        & 0xFF);
    /* success */
    colors[9]  = expand8to16((s->success >> 16) & 0xFF);
    colors[10] = expand8to16((s->success >>  8) & 0xFF);
    colors[11] = expand8to16( s->success        & 0xFF);

    xcb_change_property(p->conn, XCB_PROP_MODE_REPLACE,
                        IswWindow(p->shell), atom, XCB_ATOM_CARDINAL,
                        32, 12, colors);
}

static void tray_claim_selection(Panel *p)
{
    xcb_set_selection_owner(p->conn, IswWindow(p->shell),
                            p->atom_tray_sel, XCB_CURRENT_TIME);

    /* Verify we got it */
    xcb_get_selection_owner_reply_t *owner =
        xcb_get_selection_owner_reply(p->conn,
            xcb_get_selection_owner(p->conn, p->atom_tray_sel), NULL);
    if (!owner || owner->owner != IswWindow(p->shell)) {
        fprintf(stderr, "isde-panel: tray: failed to claim selection\n");
        free(owner);
        return;
    }
    free(owner);

    /* Set _NET_SYSTEM_TRAY_COLORS so clients use our theme colors */
    tray_set_colors(p);

    /* Announce to clients via MANAGER ClientMessage on root */
    xcb_client_message_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = p->root;
    ev.type = intern(p->conn, "MANAGER");
    ev.format = 32;
    ev.data.data32[0] = XCB_CURRENT_TIME;
    ev.data.data32[1] = p->atom_tray_sel;
    ev.data.data32[2] = IswWindow(p->shell);
    xcb_send_event(p->conn, 0, p->root,
                   XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&ev);
    xcb_flush(p->conn);

    char sel_name[32];
    snprintf(sel_name, sizeof(sel_name), "_NET_SYSTEM_TRAY_S%d",
             p->screen_num);
    fprintf(stderr, "isde-panel: tray: manager active on %s\n", sel_name);
}

void tray_init_selection(Panel *p)
{
    tray_claim_selection(p);

    /* Register Xt event handlers (once) for tray protocol events */
    IswAddRawEventHandler(p->shell, 0, True,
                          shell_event_handler, (IswPointer)p);
    IswAddEventHandler(p->tray_box,
                       XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, False,
                       traybox_event_handler, (IswPointer)p);
}

void tray_check_icons(Panel *p)
{
    int changed = 0;
    for (int i = p->ntray - 1; i >= 0; i--) {
        xcb_get_window_attributes_cookie_t ck =
            xcb_get_window_attributes(p->conn, p->tray_icons[i]);
        xcb_get_window_attributes_reply_t *r =
            xcb_get_window_attributes_reply(p->conn, ck, NULL);
        if (!r) {
            /* Window gone — remove from array */
            IswUnregisterDrawable(p->conn, p->tray_icons[i]);
            fprintf(stderr, "isde-panel: tray: icon 0x%x gone (%d remaining)\n",
                    p->tray_icons[i], p->ntray - 1);
            p->ntray--;
            for (int j = i; j < p->ntray; j++)
                p->tray_icons[j] = p->tray_icons[j + 1];
            changed = 1;
        } else {
            free(r);
        }
    }
    if (changed) {
        int icon_size = PANEL_HEIGHT - 4;
        int stride = icon_size + 2;
        int tray_w = p->ntray > 0 ? p->ntray * stride + 2 : 2;

        Arg a[20];
        Cardinal na = 0;
        IswSetArg(a[na], IswNflexBasis, tray_w); na++;
        IswSetValues(p->tray_box, a, na);

        double sf = ISWScaleFactor(p->toplevel);
        int phys_stride = (int)(stride * sf + 0.5);

        for (int i = 0; i < p->ntray; i++) {
            uint32_t xy[] = { i * phys_stride, 2 };
            xcb_configure_window(p->conn, p->tray_icons[i],
                                 XCB_CONFIG_WINDOW_X |
                                 XCB_CONFIG_WINDOW_Y, xy);
        }
        xcb_flush(p->conn);
    }
}

void tray_cleanup(Panel *p)
{
    /* Release selection */
    if (p->atom_tray_sel != XCB_ATOM_NONE) {
        xcb_set_selection_owner(p->conn, XCB_NONE,
                                p->atom_tray_sel, XCB_CURRENT_TIME);
    }

    /* Reparent icons back to root */
    for (int i = 0; i < p->ntray; i++) {
        xcb_reparent_window(p->conn, p->tray_icons[i], p->root, 0, 0);
    }

    free(p->tray_icons);
    p->tray_icons = NULL;
    p->ntray = 0;
    p->cap_tray = 0;
    xcb_flush(p->conn);
}
