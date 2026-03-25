#define _POSIX_C_SOURCE 200809L
/*
 * frame.c — window frame creation using ISW widgets
 *
 * Each frame is an OverrideShell containing:
 *   - Form (layout container)
 *     - Label (title bar — shows window name)
 *     - Command (close button)
 *   - The client window is reparented below the title area
 */
#include "wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- title fetching ---------- */

static char *fetch_title(Wm *wm, xcb_window_t win)
{
    /* Try _NET_WM_NAME first (UTF-8) */
    xcb_get_property_reply_t *reply = xcb_get_property_reply(
        wm->conn,
        xcb_get_property(wm->conn, 0, win, wm->atom_net_wm_name,
                         XCB_ATOM_ANY, 0, 256),
        NULL);
    if (reply && reply->value_len > 0) {
        char *title = strndup(xcb_get_property_value(reply),
                              reply->value_len);
        free(reply);
        return title;
    }
    free(reply);

    /* Fallback to WM_NAME */
    reply = xcb_get_property_reply(
        wm->conn,
        xcb_get_property(wm->conn, 0, win, wm->atom_wm_name,
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

/* ---------- callbacks ---------- */

static void close_callback(Widget w, XtPointer client_data,
                           XtPointer call_data)
{
    (void)w;
    (void)call_data;
    Wm *wm = ((void **)client_data)[0];
    WmClient *c = ((void **)client_data)[1];
    wm_close_client(wm, c);
}

static void maximize_callback(Widget w, XtPointer client_data,
                              XtPointer call_data)
{
    (void)w;
    (void)call_data;
    Wm *wm = ((void **)client_data)[0];
    WmClient *c = ((void **)client_data)[1];
    wm_maximize_client(wm, c);
}

static void minimize_callback(Widget w, XtPointer client_data,
                              XtPointer call_data)
{
    (void)w;
    (void)call_data;
    Wm *wm = ((void **)client_data)[0];
    WmClient *c = ((void **)client_data)[1];
    wm_minimize_client(wm, c);
}

static void title_press_callback(Widget w, XtPointer client_data,
                                 XtPointer call_data)
{
    (void)w;
    (void)call_data;
    Wm *wm = ((void **)client_data)[0];
    WmClient *c = ((void **)client_data)[1];
    wm_focus_client(wm, c);
}

/* ---------- event handlers for drag ---------- */

static void title_button_handler(Widget w, XtPointer client_data,
                                 XEvent *event, Boolean *cont)
{
    (void)w;
    (void)cont;
    Wm *wm = ((void **)client_data)[0];
    WmClient *c = ((void **)client_data)[1];

    if ((event->response_type & ~0x80) != XCB_BUTTON_PRESS)
        return;

    xcb_button_press_event_t *ev = (xcb_button_press_event_t *)event;
    wm_focus_client(wm, c);
    wm->drag_mode    = DRAG_MOVE;
    wm->drag_client  = c;
    wm->drag_start_x = ev->root_x;
    wm->drag_start_y = ev->root_y;
    wm->drag_orig_x  = c->x;
    wm->drag_orig_y  = c->y;

    /* Grab pointer to root so all motion/release events come to us
     * via the main event loop, not the widget handler */
    xcb_grab_pointer(wm->conn, 1, wm->root,
                     XCB_EVENT_MASK_BUTTON_RELEASE |
                     XCB_EVENT_MASK_POINTER_MOTION,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                     XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
    xcb_flush(wm->conn);
}

/* ---------- frame creation ---------- */

WmClient *frame_create(Wm *wm, xcb_window_t client)
{
    /* Get client geometry */
    xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(
        wm->conn, xcb_get_geometry(wm->conn, client), NULL);
    if (!geo)
        return NULL;

    WmClient *c = calloc(1, sizeof(*c));
    if (!c) { free(geo); return NULL; }

    c->client = client;
    c->x      = geo->x;
    c->y      = geo->y;
    c->width  = geo->width;
    c->height = geo->height;
    free(geo);

    c->title = fetch_title(wm, client);

    int fw = frame_total_width(c);
    int fh = frame_total_height(c);

    /* Callback closure: array of [wm, client] pointers.
     * Allocated once, freed in frame_destroy. */
    void **closure = malloc(2 * sizeof(void *));
    closure[0] = wm;
    closure[1] = c;

    /* Create OverrideShell for the frame */
    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNx, c->x);               n++;
    XtSetArg(args[n], XtNy, c->y);               n++;
    XtSetArg(args[n], XtNwidth, fw);              n++;
    XtSetArg(args[n], XtNheight, fh);             n++;
    XtSetArg(args[n], XtNoverrideRedirect, True); n++;
    XtSetArg(args[n], XtNborderWidth, 0);         n++;
    c->shell = XtCreatePopupShell("frame", overrideShellWidgetClass,
                                  wm->toplevel, args, n);

    /* All title bar widgets are direct children of the shell.
     * We position them explicitly with XtConfigureWidget — no
     * container widget (Box/Form) that could fight our layout. */

    int btn_area = 3 * WM_TITLE_HEIGHT;
    int title_w = fw - btn_area;
    if (title_w < 1) title_w = 1;

    /* Title bar label */
    n = 0;
    XtSetArg(args[n], XtNlabel, c->title ? c->title : "(untitled)"); n++;
    XtSetArg(args[n], XtNwidth, title_w);              n++;
    XtSetArg(args[n], XtNheight, WM_TITLE_HEIGHT);    n++;
    XtSetArg(args[n], XtNborderWidth, 0);              n++;
    c->title_label = XtCreateManagedWidget("titleBar", labelWidgetClass,
                                           c->shell, args, n);

    XtAddEventHandler(c->title_label,
                      ButtonPressMask,
                      False, title_button_handler, closure);

    /* Minimize button */
    n = 0;
    XtSetArg(args[n], XtNlabel, "\xe2\x80\x93");     n++; /* UTF-8 – */
    XtSetArg(args[n], XtNwidth, WM_TITLE_HEIGHT);     n++;
    XtSetArg(args[n], XtNheight, WM_TITLE_HEIGHT);    n++;
    XtSetArg(args[n], XtNborderWidth, 0);              n++;
    c->minimize_btn = XtCreateManagedWidget("minimizeBtn", commandWidgetClass,
                                            c->shell, args, n);
    XtAddCallback(c->minimize_btn, XtNcallback, minimize_callback, closure);

    /* Maximize / restore button */
    n = 0;
    XtSetArg(args[n], XtNlabel, "\xe2\x96\xa1");     n++; /* UTF-8 □ */
    XtSetArg(args[n], XtNwidth, WM_TITLE_HEIGHT);     n++;
    XtSetArg(args[n], XtNheight, WM_TITLE_HEIGHT);    n++;
    XtSetArg(args[n], XtNborderWidth, 0);              n++;
    c->maximize_btn = XtCreateManagedWidget("maximizeBtn", commandWidgetClass,
                                            c->shell, args, n);
    XtAddCallback(c->maximize_btn, XtNcallback, maximize_callback, closure);

    /* Close button */
    n = 0;
    XtSetArg(args[n], XtNlabel, "\xc3\x97");          n++; /* UTF-8 × */
    XtSetArg(args[n], XtNwidth, WM_TITLE_HEIGHT);     n++;
    XtSetArg(args[n], XtNheight, WM_TITLE_HEIGHT);    n++;
    XtSetArg(args[n], XtNborderWidth, 0);              n++;
    c->close_btn = XtCreateManagedWidget("closeBtn", commandWidgetClass,
                                         c->shell, args, n);
    XtAddCallback(c->close_btn, XtNcallback, close_callback, closure);

    /* Realize the shell so we get a window ID */
    XtRealizeWidget(c->shell);

    /* Set correct initial positions for all title bar widgets */
    frame_configure(wm, c);

    /* Reparent the client window into the frame, below the title bar */
    xcb_reparent_window(wm->conn, client, XtWindow(c->shell),
                        WM_BORDER_WIDTH, WM_TITLE_HEIGHT);

    /* Remove client border */
    uint32_t bw = 0;
    xcb_configure_window(wm->conn, client,
                         XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw);

    /* Listen for property changes on the client */
    uint32_t client_mask = XCB_EVENT_MASK_PROPERTY_CHANGE |
                           XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(wm->conn, client,
                                 XCB_CW_EVENT_MASK, &client_mask);

    /* Link into list */
    c->next = wm->clients;
    wm->clients = c;

    return c;
}

/* ---------- frame destruction ---------- */

void frame_destroy(Wm *wm, WmClient *c)
{
    /* Reparent client back to root */
    xcb_reparent_window(wm->conn, c->client, wm->root, c->x, c->y);
    xcb_flush(wm->conn);

    if (c->shell)
        XtDestroyWidget(c->shell);

    free(c->title);
    free(c);
}

/* ---------- frame geometry ---------- */

int frame_total_width(WmClient *c)
{
    return c->width + 2 * WM_BORDER_WIDTH;
}

int frame_total_height(WmClient *c)
{
    return c->height + WM_TITLE_HEIGHT + WM_BORDER_WIDTH;
}

/* ---------- reconfigure frame + client ---------- */

void frame_configure(Wm *wm, WmClient *c)
{
    int fw = frame_total_width(c);
    int fh = frame_total_height(c);
    int btn_area = 3 * WM_TITLE_HEIGHT;

    /* Use XtConfigureWidget to update both Xt internal state and
     * the X window atomically — avoids Xt and XCB disagreeing */
    XtConfigureWidget(c->shell, c->x, c->y, fw, fh, 0);

    /* Position title bar widgets directly within the shell */
    int title_w = fw - btn_area;
    if (title_w < 1) title_w = 1;
    XtConfigureWidget(c->title_label, 0, 0,
                      title_w, WM_TITLE_HEIGHT, 0);
    XtConfigureWidget(c->minimize_btn, title_w, 0,
                      WM_TITLE_HEIGHT, WM_TITLE_HEIGHT, 0);
    XtConfigureWidget(c->maximize_btn, title_w + WM_TITLE_HEIGHT, 0,
                      WM_TITLE_HEIGHT, WM_TITLE_HEIGHT, 0);
    XtConfigureWidget(c->close_btn, title_w + 2 * WM_TITLE_HEIGHT, 0,
                      WM_TITLE_HEIGHT, WM_TITLE_HEIGHT, 0);

    /* Resize client window */
    uint32_t cvals[] = { c->width, c->height };
    xcb_configure_window(wm->conn, c->client,
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         cvals);
    xcb_flush(wm->conn);
}

/* ---------- title update ---------- */

void frame_update_title(Wm *wm, WmClient *c)
{
    (void)wm;
    free(c->title);
    c->title = fetch_title(wm, c->client);

    Arg args[20];
    XtSetArg(args[0], XtNlabel, c->title ? c->title : "(untitled)");
    XtSetValues(c->title_label, args, 1);
}
