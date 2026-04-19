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
#include <ISW/IswArgMacros.h>

#define GRIP_SIZE 6

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
    if (!reply) {
        return wm->screen->white_pixel;
    }
    Pixel px = reply->pixel;
    free(reply);
    return px;
}

/* Apply theme colors to a client's frame widgets */
/* Only the title bar needs IswSetValues — focused/unfocused is state-dependent.
 * Everything else comes from Xresources. */
void frame_apply_theme(Wm *wm, WmClient *c)
{
    if (!c->decorated) {
        return;
    }

    const IsdeColorScheme *s = isde_theme_current();
    if (!s) {
        return;
    }

    const IsdeElementColors *tb = c->focused
        ? &s->titlebar_active : &s->titlebar;

    /* Include explicit width/height so IswSetValues doesn't resize the
     * label to its preferred (text-fitting) geometry. */
    int th = wm->title_height;
    int btn_area = 3 * th;
    int title_w = c->width - btn_area;
    if (title_w < 1) { title_w = 1; }

    Arg args[20];
    Cardinal n = 0;
    IswSetArg(args[n], IswNbackground, color_to_pixel(wm, tb->bg)); n++;
    IswSetArg(args[n], IswNforeground, color_to_pixel(wm, tb->fg)); n++;
    IswSetArg(args[n], IswNwidth, title_w);                          n++;
    IswSetArg(args[n], IswNheight, th);                              n++;
    IswSetValues(c->title_label, args, n);
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

static void close_callback(Widget w, IswPointer client_data,
                           IswPointer call_data)
{
    (void)w;
    (void)call_data;
    Wm *wm = ((void **)client_data)[0];
    WmClient *c = ((void **)client_data)[1];
    wm_close_client(wm, c);
}

static void maximize_callback(Widget w, IswPointer client_data,
                              IswPointer call_data)
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
                    Arg a[20];
                    Cardinal an = 0;
                    IswSetArg(a[an], IswNimage, data);          an++;
                    IswSetArg(a[an], IswNwidth, wm->title_height);  an++;
                    IswSetArg(a[an], IswNheight, wm->title_height); an++;
                    IswSetValues(c->maximize_btn, a, an);
                    free(data);
                }
                fclose(fp);
            }
        }
    }
}

static void minimize_callback(Widget w, IswPointer client_data,
                              IswPointer call_data)
{
    (void)w;
    (void)call_data;
    Wm *wm = ((void **)client_data)[0];
    WmClient *c = ((void **)client_data)[1];
    wm_minimize_client(wm, c);
}

static void title_press_callback(Widget w, IswPointer client_data,
                                 IswPointer call_data)
{
    (void)w;
    (void)call_data;
    Wm *wm = ((void **)client_data)[0];
    WmClient *c = ((void **)client_data)[1];
    wm_focus_client(wm, c);
}

/* ---------- event handlers for drag ---------- */

static void title_button_handler(Widget w, IswPointer client_data,
                                 xcb_generic_event_t *event, Boolean *cont)
{
    (void)w;
    (void)cont;
    Wm *wm = ((void **)client_data)[0];
    WmClient *c = ((void **)client_data)[1];

    if ((event->response_type & ~0x80) != XCB_BUTTON_PRESS) {
        return;
    }

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

/* Command widget default translations lack LeaveWindow/EnterWindow bindings,
 * so the button stays "set" when the pointer leaves and fires on release
 * anywhere.  Override with the missing bindings so that leaving the button
 * cancels the press. */
static IswTranslations btn_leave_fixup;

/* ---------- frame creation ---------- */

WmClient *frame_create(Wm *wm, xcb_window_t client)
{
    frame_init_icons();

    if (!btn_leave_fixup) {
        btn_leave_fixup = IswParseTranslationTable(
            "<LeaveWindow>: reset()\n"
            "<EnterWindow>: highlight(Always)");
    }

    /* Get client geometry */
    xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(
        wm->conn, xcb_get_geometry(wm->conn, client), NULL);
    if (!geo) {
        return NULL;
    }

    WmClient *c = calloc(1, sizeof(*c));
    if (!c) { free(geo); return NULL; }

    c->client = client;
    /* xcb_get_geometry returns physical pixels — convert to logical */
    double sf = wm->scale_factor;
    c->x      = (int)(geo->x / sf + 0.5);
    c->y      = (int)(geo->y / sf + 0.5);
    c->width  = (int)(geo->width / sf + 0.5);
    c->height = (int)(geo->height / sf + 0.5);
    free(geo);

    c->decorated = wm_client_wants_decorations(wm, client) &&
                   wm_window_type_wants_decorations(wm, client);

    c->title = fetch_title(wm, client);

    /* Smart placement: center transients over parent, cascade others */
    wm_place_client(wm, c);

    int fw = frame_total_width(c);
    int fh = frame_total_height(wm, c);

    /* Callback closure: array of [wm, client] pointers.
     * Allocated once, freed in frame_destroy. */
    void **closure = malloc(2 * sizeof(void *));
    closure[0] = wm;
    closure[1] = c;

    /* Create OverrideShell for the frame.  All geometry is logical;
       ISW scales to physical when creating the X window. */
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgX(&ab, c->x);
    IswArgY(&ab, c->y);
    IswArgWidth(&ab, fw);
    IswArgHeight(&ab, fh);
    IswArgOverrideRedirect(&ab, True);
    IswArgBorderWidth(&ab, 1);
    {
        const IsdeColorScheme *s = isde_theme_current();
        if (s) {
            IswArgBorderColor(&ab, color_to_pixel(wm, s->fg));
        }
    }
    c->shell = IswCreatePopupShell("frame", overrideShellWidgetClass,
                                  wm->toplevel, ab.args, ab.count);

    /* Title bar widgets — only for decorated windows */
    if (c->decorated) {
        int btn_area = 3 * wm->title_height;
        int title_w = fw - btn_area;
        if (title_w < 1) { title_w = 1; }

        /* Title bar label */
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, c->title ? c->title : "(untitled)");
        IswArgWidth(&ab, title_w);
        IswArgHeight(&ab, WM_TITLE_HEIGHT);
        IswArgBorderWidth(&ab, 0);
        IswArgResize(&ab, False);
        c->title_label = IswCreateWidget("titleBar", labelWidgetClass,
                                        c->shell, ab.args, ab.count);

        IswAddEventHandler(c->title_label,
                          XCB_EVENT_MASK_BUTTON_PRESS,
                          False, title_button_handler, closure);

        /* Minimize button */
        IswArgBuilderReset(&ab);
        if (icon_minimize) {
            IswArgImage(&ab, icon_minimize);
        }
        IswArgWidth(&ab, WM_TITLE_HEIGHT);
        IswArgHeight(&ab, WM_TITLE_HEIGHT);
        IswArgInternalWidth(&ab, 0);
        IswArgInternalHeight(&ab, 0);
        IswArgCornerRadius(&ab, 0);
        c->minimize_btn = IswCreateWidget("minimizeBtn", commandWidgetClass,
                                         c->shell, ab.args, ab.count);
        IswOverrideTranslations(c->minimize_btn, btn_leave_fixup);
        IswAddCallback(c->minimize_btn, IswNcallback, minimize_callback, closure);

        /* Maximize / restore button */
        IswArgBuilderReset(&ab);
        if (icon_maximize) {
            IswArgImage(&ab, icon_maximize);
        }
        IswArgWidth(&ab, WM_TITLE_HEIGHT);
        IswArgHeight(&ab, WM_TITLE_HEIGHT);
        IswArgInternalWidth(&ab, 0);
        IswArgInternalHeight(&ab, 0);
        IswArgCornerRadius(&ab, 0);
        c->maximize_btn = IswCreateWidget("maximizeBtn", commandWidgetClass,
                                         c->shell, ab.args, ab.count);
        IswOverrideTranslations(c->maximize_btn, btn_leave_fixup);
        IswAddCallback(c->maximize_btn, IswNcallback, maximize_callback, closure);

        /* Close button */
        IswArgBuilderReset(&ab);
        if (icon_close) {
            IswArgImage(&ab, icon_close);
        }
        IswArgWidth(&ab, WM_TITLE_HEIGHT);
        IswArgHeight(&ab, WM_TITLE_HEIGHT);
        IswArgInternalWidth(&ab, 0);
        IswArgInternalHeight(&ab, 0);
        IswArgCornerRadius(&ab, 0);
        c->close_btn = IswCreateWidget("closeBtn", commandWidgetClass,
                                      c->shell, ab.args, ab.count);
        IswOverrideTranslations(c->close_btn, btn_leave_fixup);
        IswAddCallback(c->close_btn, IswNcallback, close_callback, closure);
    }

    /* Realize the shell so we get a window ID.  Title bar widgets are
     * created unmanaged so Shell's Resize proc never touches them —
     * map them explicitly after realization. */
    frame_init_cursors(wm);
    IswRealizeWidget(c->shell);
    if (c->decorated) {
        IswMapWidget(c->title_label);
        IswMapWidget(c->minimize_btn);
        IswMapWidget(c->maximize_btn);
        IswMapWidget(c->close_btn);
    }

    /* Set correct initial positions and apply theme colors */
    frame_configure(wm, c);
    frame_apply_theme(wm, c);

    /* Add client to the save-set before reparenting.
     * If the WM connection closes for any reason (crash, SIGKILL),
     * X automatically reparents and maps save-set windows to root. */
    xcb_change_save_set(wm->conn, XCB_SET_MODE_INSERT, client);

    /* Reparent the client window into the frame, below the title bar.
     * Client keeps its full requested size; border is extra space around it.
     * xcb_reparent_window needs physical pixel offsets. */
    {
        double sf = wm->scale_factor;
        int title = c->decorated ? wm->title_height : 0;
        int phys_bw = (int)(WM_BORDER_WIDTH * sf + 0.5);
        int phys_title = (int)(title * sf + 0.5);
        xcb_reparent_window(wm->conn, client, IswWindow(c->shell),
                            phys_bw, phys_bw + phys_title);
    }

    /* Create invisible resize grips — after reparent so they stack on top.
     * Undecorated windows handle their own resize. */
    if (c->decorated) {
        frame_create_grips(wm, c);
    }

    /* Remove client border */
    uint32_t bw = 0;
    xcb_configure_window(wm->conn, client,
                         XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw);

    /* Listen for property changes on the client */
    uint32_t client_mask = XCB_EVENT_MASK_PROPERTY_CHANGE |
                           XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(wm->conn, client,
                                 XCB_CW_EVENT_MASK, &client_mask);

    /* Passive grab for click-to-focus on the client window.
     * SYNC grab freezes the pointer until we replay the event,
     * ensuring the WM can focus+raise before the client sees it. */
    xcb_grab_button(wm->conn, 0, client,
                    XCB_EVENT_MASK_BUTTON_PRESS,
                    XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                    XCB_NONE, XCB_NONE,
                    XCB_BUTTON_INDEX_1, XCB_MOD_MASK_ANY);

    /* Append to list tail — _NET_CLIENT_LIST must be in initial mapping order */
    c->next = NULL;
    if (!wm->clients) {
        wm->clients = c;
    } else {
        WmClient *tail = wm->clients;
        while (tail->next)
            tail = tail->next;
        tail->next = c;
    }

    return c;
}

/* ---------- frame destruction ---------- */

void frame_destroy(Wm *wm, WmClient *c)
{
    frame_destroy_grips(wm, c);

    /* Remove from save-set, reparent back to root, and re-map.
     * X auto-unmaps a mapped window during reparent, so we re-map it
     * unless it was intentionally minimized. */
    xcb_change_save_set(wm->conn, XCB_SET_MODE_DELETE, c->client);
    /* Reparent back to root — xcb needs physical pixel coords */
    double sf = wm->scale_factor;
    xcb_reparent_window(wm->conn, c->client, wm->root,
                        (int)(c->x * sf + 0.5), (int)(c->y * sf + 0.5));
    if (!c->minimized) {
        xcb_map_window(wm->conn, c->client);
    }
    xcb_flush(wm->conn);

    if (c->shell) {
        IswDestroyWidget(c->shell);
    }

    free(c->title);
    free(c);
}

/* ---------- frame geometry ---------- */

int frame_total_width(WmClient *c)
{
    return c->width + 2 * WM_BORDER_WIDTH;
}

int frame_total_height(Wm *wm, WmClient *c)
{
    int title = c->decorated ? wm->title_height : 0;
    return c->height + title + 2 * WM_BORDER_WIDTH;
}

/* ---------- reconfigure frame + client ---------- */

void frame_configure(Wm *wm, WmClient *c)
{
    int fw = frame_total_width(c);
    int fh = frame_total_height(wm, c);
    int th = wm->title_height;
    int title = c->decorated ? th : 0;

    /* Use IswConfigureWidget to update both Xt internal state and
     * the X window atomically — avoids Xt and XCB disagreeing.
     * Non-maximized windows get a 1px border; maximized get none. */
    int bw = c->maximized ? 0 : 1;
    IswConfigureWidget(c->shell, c->x, c->y, fw, fh, bw);

    /* IswConfigureWidget may skip the border_width change if Xt thinks
     * it hasn't changed, so force it via XCB as well (physical pixels). */
    double sf = wm->scale_factor;
    uint32_t bw32 = log_to_phys(sf, bw);
    xcb_configure_window(wm->conn, IswWindow(c->shell),
                         XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw32);

    if (c->decorated) {
        int btn_area = 3 * th;
        int inner_w = c->width;
        int title_w = inner_w - btn_area;
        if (title_w < 1) { title_w = 1; }
        int bx = WM_BORDER_WIDTH;
        int by = WM_BORDER_WIDTH;
        IswConfigureWidget(c->title_label, bx, by,
                          title_w, th, 0);
        IswConfigureWidget(c->minimize_btn, bx + title_w, by,
                          th, th, 0);
        IswConfigureWidget(c->maximize_btn, bx + title_w + th, by,
                          th, th, 0);
        IswConfigureWidget(c->close_btn, bx + title_w + 2 * th, by,
                          th, th, 0);
    }

    /* Reposition client window within the frame (physical pixels for XCB) */
    int phys_bw = (int)(WM_BORDER_WIDTH * sf + 0.5);
    int phys_title = (int)(title * sf + 0.5);
    uint32_t cpos[] = { phys_bw, phys_bw + phys_title };
    xcb_configure_window(wm->conn, c->client,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                         cpos);

    /* Resize client window (physical pixels for XCB) */
    uint32_t cvals[] = { (uint32_t)(c->width * sf + 0.5),
                         (uint32_t)(c->height * sf + 0.5) };
    xcb_configure_window(wm->conn, c->client,
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         cvals);

    /* Update grip positions */
    if (c->grip[0]) {
        frame_update_grips(wm, c);
    }

    xcb_flush(wm->conn);
}

/* ---------- title update ---------- */

void frame_update_title(Wm *wm, WmClient *c)
{
    free(c->title);
    c->title = fetch_title(wm, c->client);

    if (c->title_label) {
        int th = wm->title_height;
        int btn_area = 3 * th;
        int title_w = c->width - btn_area;
        if (title_w < 1) { title_w = 1; }

        Arg args[20];
        Cardinal n = 0;
        IswSetArg(args[n], IswNlabel, c->title ? c->title : "(untitled)"); n++;
        IswSetArg(args[n], IswNwidth, title_w);                             n++;
        IswSetArg(args[n], IswNheight, th);                                 n++;
        IswSetValues(c->title_label, args, n);
    }
}

/* ---------- resize cursors ---------- */

void frame_init_cursors(Wm *wm)
{
    if (wm->cursors[0]) {
        return;
    }
    xcb_cursor_context_t *ctx;
    if (xcb_cursor_context_new(wm->conn, wm->screen, &ctx) < 0) {
        return;
    }
    static const char *names[8] = {
        "top_side", "bottom_side", "left_side", "right_side",
        "top_left_corner", "top_right_corner",
        "bottom_left_corner", "bottom_right_corner"
    };
    for (int i = 0; i < 8; i++) {
        wm->cursors[i] = xcb_cursor_load_cursor(ctx, names[i]);
    }
    xcb_cursor_context_free(ctx);
}

/* ---------- resize grips ---------- */

void frame_create_grips(Wm *wm, WmClient *c)
{
    xcb_window_t parent = IswWindow(c->shell);
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
    /* Grips are raw XCB children of the frame shell window, so their
     * coordinates must be in physical pixels. */
    double sf = wm->scale_factor;
    int fw = log_to_phys(sf, frame_total_width(c));
    int fh = log_to_phys(sf, frame_total_height(wm, c));
    int g = log_to_phys(sf, GRIP_SIZE);
    int th = log_to_phys(sf, wm->title_height);

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
