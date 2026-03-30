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
#include <xcb/xcb_cursor.h>

#define GRIP_SIZE isde_scale(6)

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
                                 xcb_generic_event_t *event, Boolean *cont)
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

    /* Smart placement: center transients over parent, cascade others */
    wm_place_client(wm, c);

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
    frame_init_cursors(wm);
    XtRealizeWidget(c->shell);

    /* Set correct initial positions and apply theme colors */
    frame_configure(wm, c);
    frame_apply_theme(wm, c);

    /* Reparent the client window into the frame, below the title bar.
     * Client keeps its full requested size; border is extra space around it. */
    xcb_reparent_window(wm->conn, client, XtWindow(c->shell),
                        WM_BORDER_WIDTH,
                        WM_BORDER_WIDTH + WM_TITLE_HEIGHT);

    /* Create invisible resize grips — after reparent so they stack on top */
    frame_create_grips(wm, c);

    /* Remove client border */
    uint32_t bw = 0;
    xcb_configure_window(wm->conn, client,
                         XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw);

    /* Listen for property changes on the client */
    uint32_t client_mask = XCB_EVENT_MASK_PROPERTY_CHANGE |
                           XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(wm->conn, client,
                                 XCB_CW_EVENT_MASK, &client_mask);

    /* Passive grab for click-to-focus on the client window */
    xcb_grab_button(wm->conn, 0, client,
                    XCB_EVENT_MASK_BUTTON_PRESS,
                    XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                    XCB_NONE, XCB_NONE,
                    XCB_BUTTON_INDEX_1, XCB_MOD_MASK_ANY);

    /* Link into list */
    c->next = wm->clients;
    wm->clients = c;

    return c;
}

/* ---------- frame destruction ---------- */

void frame_destroy(Wm *wm, WmClient *c)
{
    frame_destroy_grips(wm, c);

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

    /* Update grip positions */
    if (c->grip[0])
        frame_update_grips(wm, c);

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

/* ---------- resize cursors ---------- */

void frame_init_cursors(Wm *wm)
{
    if (wm->cursors[0]) return;
    xcb_cursor_context_t *ctx;
    if (xcb_cursor_context_new(wm->conn, wm->screen, &ctx) < 0)
        return;
    static const char *names[8] = {
        "top_side", "bottom_side", "left_side", "right_side",
        "top_left_corner", "top_right_corner",
        "bottom_left_corner", "bottom_right_corner"
    };
    for (int i = 0; i < 8; i++)
        wm->cursors[i] = xcb_cursor_load_cursor(ctx, names[i]);
    xcb_cursor_context_free(ctx);
}

/* ---------- resize grips ---------- */

void frame_create_grips(Wm *wm, WmClient *c)
{
    xcb_window_t parent = XtWindow(c->shell);
    uint32_t mask = XCB_EVENT_MASK_BUTTON_PRESS |
                    XCB_EVENT_MASK_ENTER_WINDOW |
                    XCB_EVENT_MASK_LEAVE_WINDOW;

    for (int i = 0; i < 8; i++) {
        uint32_t vals[2] = { mask, wm->cursors[i] };
        c->grip[i] = xcb_generate_id(wm->conn);
        xcb_create_window(wm->conn, 0, c->grip[i], parent,
                          0, 0, 1, 1, 0,
                          XCB_WINDOW_CLASS_INPUT_ONLY,
                          XCB_COPY_FROM_PARENT,
                          XCB_CW_EVENT_MASK | XCB_CW_CURSOR, vals);
        xcb_map_window(wm->conn, c->grip[i]);
    }
    frame_update_grips(wm, c);
}

void frame_update_grips(Wm *wm, WmClient *c)
{
    int fw = frame_total_width(c);
    int fh = frame_total_height(c);
    int g = GRIP_SIZE;
    int th = WM_TITLE_HEIGHT;

    /* Grips sit on client edges, below the title bar.
     * No top edge grip — title bar handles that area.
     * Left/right grips span from title bar bottom to frame bottom.
     * Corner grips at bottom-left and bottom-right only. */
    int client_top = th;
    int client_h = fh - th;

    /* Order: top, bottom, left, right, tl, tr, bl, br
     * Top and top-corners are zero-sized (disabled) since
     * the title bar occupies that space. */
    struct { int x, y, w, h; } r[8] = {
        { 0,      0,        0, 0 },                      /* top — disabled */
        { g,      fh - g,   fw - 2*g, g },               /* bottom */
        { 0,      client_top, g, client_h - g },          /* left */
        { fw - g, client_top, g, client_h - g },          /* right */
        { 0,      0,        0, 0 },                       /* tl — disabled */
        { 0,      0,        0, 0 },                       /* tr — disabled */
        { 0,      fh - g,   g, g },                       /* bottom-left */
        { fw - g, fh - g,   g, g },                       /* bottom-right */
    };

    for (int i = 0; i < 8; i++) {
        if (r[i].w < 1 || r[i].h < 1) {
            xcb_unmap_window(wm->conn, c->grip[i]);
            continue;
        }
        xcb_map_window(wm->conn, c->grip[i]);
        uint32_t vals[] = { r[i].x, r[i].y, r[i].w, r[i].h,
                            XCB_STACK_MODE_ABOVE };
        xcb_configure_window(wm->conn, c->grip[i],
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                             XCB_CONFIG_WINDOW_STACK_MODE, vals);
    }
}

void frame_destroy_grips(Wm *wm, WmClient *c)
{
    for (int i = 0; i < 8; i++) {
        if (c->grip[i]) {
            xcb_destroy_window(wm->conn, c->grip[i]);
            c->grip[i] = 0;
        }
    }
}
