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

/* Convert 0xRRGGBB to an X11 Pixel via AllocColor */
static Pixel color_to_pixel(Wm *wm, unsigned int rgb)
{
    xcb_alloc_color_reply_t *reply = xcb_alloc_color_reply(
        wm->conn,
        xcb_alloc_color(wm->conn, wm->screen->default_colormap,
                        ((rgb >> 16) & 0xFF) * 257,
                        ((rgb >> 8)  & 0xFF) * 257,
                        ( rgb        & 0xFF) * 257),
        NULL);
    if (!reply) return wm->screen->white_pixel;
    Pixel px = reply->pixel;
    free(reply);
    return px;
}

/* Apply theme colors to a client's frame widgets */
/* Only the title bar needs XtSetValues — focused/unfocused is state-dependent.
 * Everything else comes from Xresources. */
void frame_apply_theme(Wm *wm, WmClient *c)
{
    const IsdeColorScheme *s = isde_theme_current();
    if (!s) return;

    const IsdeElementColors *tb = c->focused
        ? &s->titlebar_active : &s->titlebar;

    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNbackground, color_to_pixel(wm, tb->bg)); n++;
    XtSetArg(args[n], XtNforeground, color_to_pixel(wm, tb->fg)); n++;
    XtSetValues(c->title_label, args, n);
}

/* ---------- icon paths (resolved once) ---------- */

static char *icon_minimize;
static char *icon_maximize;
static char *icon_restore;
static char *icon_close;

static void frame_init_icons(void)
{
    free(icon_minimize);
    free(icon_maximize);
    free(icon_restore);
    free(icon_close);
    icon_minimize = isde_icon_find("actions", "window-minimize");
    icon_maximize = isde_icon_find("actions", "window-maximize");
    icon_restore  = isde_icon_find("actions", "window-restore");
    icon_close    = isde_icon_find("actions", "window-close");
}

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

    /* Swap icon using svgData (svgFile SetValues doesn't redraw) */
    {
        char *icon = c->maximized ? icon_restore : icon_maximize;
        if (icon) {
            FILE *fp = fopen(icon, "r");
            if (fp) {
                fseek(fp, 0, SEEK_END);
                long len = ftell(fp);
                fseek(fp, 0, SEEK_SET);
                char *data = malloc(len + 1);
                if (data) {
                    fread(data, 1, len, fp);
                    data[len] = '\0';
                    Arg a[1];
                    XtSetArg(a[0], XtNsvgData, data);
                    XtSetValues(c->maximize_btn, a, 1);
                    free(data);
                }
                fclose(fp);
            }
        }
    }
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
    frame_init_icons();

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
    if (icon_minimize)
        { XtSetArg(args[n], XtNsvgFile, icon_minimize); n++; }
    XtSetArg(args[n], XtNwidth, WM_TITLE_HEIGHT);     n++;
    XtSetArg(args[n], XtNheight, WM_TITLE_HEIGHT);    n++;
    XtSetArg(args[n], XtNinternalWidth, 0);            n++;
    XtSetArg(args[n], XtNinternalHeight, 0);           n++;
    c->minimize_btn = XtCreateManagedWidget("minimizeBtn", commandWidgetClass,
                                            c->shell, args, n);
    XtAddCallback(c->minimize_btn, XtNcallback, minimize_callback, closure);

    /* Maximize / restore button */
    n = 0;
    if (icon_maximize)
        { XtSetArg(args[n], XtNsvgFile, icon_maximize); n++; }
    XtSetArg(args[n], XtNwidth, WM_TITLE_HEIGHT);     n++;
    XtSetArg(args[n], XtNheight, WM_TITLE_HEIGHT);    n++;
    XtSetArg(args[n], XtNinternalWidth, 0);            n++;
    XtSetArg(args[n], XtNinternalHeight, 0);           n++;
    c->maximize_btn = XtCreateManagedWidget("maximizeBtn", commandWidgetClass,
                                            c->shell, args, n);
    XtAddCallback(c->maximize_btn, XtNcallback, maximize_callback, closure);

    /* Close button */
    n = 0;
    if (icon_close)
        { XtSetArg(args[n], XtNsvgFile, icon_close); n++; }
    XtSetArg(args[n], XtNwidth, WM_TITLE_HEIGHT);     n++;
    XtSetArg(args[n], XtNheight, WM_TITLE_HEIGHT);    n++;
    XtSetArg(args[n], XtNinternalWidth, 0);            n++;
    XtSetArg(args[n], XtNinternalHeight, 0);           n++;
    c->close_btn = XtCreateManagedWidget("closeBtn", commandWidgetClass,
                                         c->shell, args, n);
    XtAddCallback(c->close_btn, XtNcallback, close_callback, closure);

    /* Realize the shell so we get a window ID */
    XtRealizeWidget(c->shell);

    /* Set correct initial positions and apply theme colors */
    frame_configure(wm, c);
    frame_apply_theme(wm, c);

    /* Reparent the client window into the frame, below the title bar.
     * Client keeps its full requested size; border is extra space around it. */
    xcb_reparent_window(wm->conn, client, XtWindow(c->shell),
                        WM_BORDER_WIDTH,
                        WM_BORDER_WIDTH + WM_TITLE_HEIGHT);

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
    return c->height + WM_TITLE_HEIGHT + 2 * WM_BORDER_WIDTH;
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

    /* Title bar inset by border width; spans inner width */
    int inner_w = c->width;
    int title_w = inner_w - btn_area;
    if (title_w < 1) title_w = 1;
    int bx = WM_BORDER_WIDTH;
    int by = WM_BORDER_WIDTH;
    XtConfigureWidget(c->title_label, bx, by,
                      title_w, WM_TITLE_HEIGHT, 0);
    XtConfigureWidget(c->minimize_btn, bx + title_w, by,
                      WM_TITLE_HEIGHT, WM_TITLE_HEIGHT, 0);
    XtConfigureWidget(c->maximize_btn, bx + title_w + WM_TITLE_HEIGHT, by,
                      WM_TITLE_HEIGHT, WM_TITLE_HEIGHT, 0);
    XtConfigureWidget(c->close_btn, bx + title_w + 2 * WM_TITLE_HEIGHT, by,
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
