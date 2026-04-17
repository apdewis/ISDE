#define _POSIX_C_SOURCE 200809L
/*
 * dnd.c — XDND drag-and-drop for the file manager
 *
 * Registers both IconView and ListView as drag sources; the shell
 * is the drop target.
 * Drag: selected files can be dragged to other windows/apps.
 * Drop: files from other windows/apps can be dropped into the
 *        current directory.
 *
 * Drag initiation uses Xt translation overrides that chain with the
 * view widget's own SelectItem/BandDrag actions.
 *
 * Uses standard XDND v5 move semantics: the drop target always
 * copies; the drag source deletes originals on move completion.
 */
#include "fm.h"

#include <ISW/ISWRender.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <xcb/xcb.h>

#define DND_THRESHOLD 5  /* pixels of motion before drag starts */

/* ---------- drag path helpers ---------- */

static void free_drag_paths(Fm *fm)
{
    for (int i = 0; i < fm->dnd_ndrag_paths; i++) {
        free(fm->dnd_drag_paths[i]);
    }
    free(fm->dnd_drag_paths);
    fm->dnd_drag_paths = NULL;
    fm->dnd_ndrag_paths = 0;
}

/* ---------- drag source: convert callback ---------- */

static Boolean drag_convert(Widget w, xcb_atom_t target_type,
                            IswPointer *data_return,
                            unsigned long *length_return,
                            int *format_return,
                            IswPointer client_data)
{
    (void)w;
    Fm *fm = (Fm *)client_data;
    Widget dnd_w = (fm->view_mode == FM_VIEW_LIST) ? fm->listview : fm->iconview;
    xcb_atom_t uri_atom = ISWXdndInternType(dnd_w, "text/uri-list");

    if (target_type != uri_atom) {
        return False;
    }

    if (fm->dnd_ndrag_paths <= 0) {
        return False;
    }

    size_t total = 0;
    for (int i = 0; i < fm->dnd_ndrag_paths; i++) {
        total += 7 + strlen(fm->dnd_drag_paths[i]) + 2;
    }

    char *buf = IswMalloc(total + 1);
    char *p = buf;
    for (int i = 0; i < fm->dnd_ndrag_paths; i++) {
        p += sprintf(p, "file://%s\r\n", fm->dnd_drag_paths[i]);
    }

    *data_return = (IswPointer)buf;
    *length_return = p - buf;
    *format_return = 8;
    return True;
}

/* ---------- drag source: finished callback ---------- */

static void drag_finished(Widget w, IswDndAction action,
                          Boolean accepted, IswPointer client_data)
{
    (void)w;
    Fm *fm = (Fm *)client_data;

    if (accepted && action == ISW_DND_ACTION_MOVE &&
        !fm->dnd_drop_was_noop && fm->dnd_drag_paths) {
        for (int i = 0; i < fm->dnd_ndrag_paths; i++) {
            struct stat st;
            if (lstat(fm->dnd_drag_paths[i], &st) == 0) {
                fileops_delete(fm, fm->dnd_drag_paths[i]);
            }
        }
        fm_refresh(fm);
    }

    free_drag_paths(fm);
}

/* ---------- drag source: initiate ---------- */

static void start_drag(Fm *fm)
{
    free_drag_paths(fm);
    fm->dnd_drop_was_noop = False;

    /* Snapshot selected file paths for convert + finished callbacks.
     * By this point SelectItem has already run on the Btn1Down event,
     * so the selection reflects the user's click. */
    int *indices = NULL;
    int nsel = fileview_get_selected_items(fm, &indices);
    if (nsel <= 0) {
        free(indices);
        return;
    }

    fm->dnd_drag_paths = malloc(nsel * sizeof(char *));
    for (int i = 0; i < nsel; i++) {
        int idx = indices[i];
        if (idx >= 0 && idx < fm->nentries) {
            fm->dnd_drag_paths[fm->dnd_ndrag_paths++] =
                strdup(fm->entries[idx].full_path);
        }
    }
    free(indices);

    if (fm->dnd_ndrag_paths <= 0) {
        free_drag_paths(fm);
        return;
    }

    static xcb_atom_t uri_type;
    Widget drag_w = (fm->view_mode == FM_VIEW_LIST) ? fm->listview : fm->iconview;
    uri_type = ISWXdndInternType(drag_w, "text/uri-list");

    IswDragSourceDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.types       = &uri_type;
    desc.num_types   = 1;
    desc.actions     = ISW_DND_ACTION_COPY | ISW_DND_ACTION_MOVE;
    desc.convert     = drag_convert;
    desc.finished    = drag_finished;
    desc.client_data = fm;

    ISWXdndStartDrag(drag_w, &fm->dnd_saved_press, &desc);
}

/* ---------- Xt action procedures for drag initiation ---------- */

/*
 * Chained before the IconView's own actions via translation overrides:
 *   <Btn1Down>:   fm-dnd-press() SelectItem()
 *   <Btn1Motion>: fm-dnd-check() BandDrag()
 *
 * fm-dnd-press saves the button event; fm-dnd-check tests the drag
 * threshold and starts the drag if a selection exists.  If
 * ISWXdndStartDrag grabs the pointer, BandDrag becomes a no-op.
 * If the click was on empty space (rubber band), there's no selection
 * so fm-dnd-check does nothing and BandDrag handles it.
 */

static void act_dnd_press(Widget w, xcb_generic_event_t *ev,
                          String *params, Cardinal *num_params)
{
    (void)params; (void)num_params;
    Fm *fm = fm_from_widget(w);
    if (!fm) { return; }

    uint8_t type = ev->response_type & ~0x80;
    if (type != XCB_BUTTON_PRESS) {
        fm->dnd_press_valid = False;
        return;
    }

    xcb_button_press_event_t *bp = (xcb_button_press_event_t *)ev;
    if (bp->detail != 1) {
        fm->dnd_press_valid = False;
        return;
    }

    memcpy(&fm->dnd_saved_press, bp, sizeof(fm->dnd_saved_press));
    fm->dnd_press_valid = True;
}

static void act_dnd_check(Widget w, xcb_generic_event_t *ev,
                          String *params, Cardinal *num_params)
{
    (void)params; (void)num_params;
    Fm *fm = fm_from_widget(w);
    if (!fm) { return; }

    if (!fm->dnd_press_valid) {
        return;
    }

    uint8_t type = ev->response_type & ~0x80;
    if (type != XCB_MOTION_NOTIFY) {
        return;
    }

    xcb_motion_notify_event_t *motion = (xcb_motion_notify_event_t *)ev;
    int dx = motion->event_x - fm->dnd_saved_press.event_x;
    int dy = motion->event_y - fm->dnd_saved_press.event_y;
    if (dx * dx + dy * dy < DND_THRESHOLD * DND_THRESHOLD) {
        return;
    }

    /* Only start drag if something is selected (not rubber band) */
    int sel = fileview_get_selected(fm);
    if (sel < 0) {
        return;
    }

    /* If the ListView is mid-band-select, let BandDrag handle it */
    if (fm->view_mode == FM_VIEW_LIST && fm->listview &&
        IswListViewBandActive(fm->listview)) {
        return;
    }

    /* Use the motion event's timestamp for the grab — Xephyr rejects
     * the original press timestamp because it matches the passive
     * grab's activation time (not strictly "later than"). */
    fm->dnd_saved_press.time = motion->time;

    fm->dnd_press_valid = False;
    start_drag(fm);
}

static IswActionsRec dnd_actions[] = {
    {"fm-dnd-press", act_dnd_press},
    {"fm-dnd-check", act_dnd_check},
};

/* ---------- URI extraction helper ---------- */

/* Extract file paths from drop callback data.  ISW populates d->uris
 * when the selection type matches text/uri-list.  For intra-process
 * drops the reported type may differ, so fall back to parsing d->data. */
static int extract_uris(IswDropCallbackData *d,
                        char ***local_out, const char *const **uris_out)
{
    *local_out = NULL;

    if (d->num_uris > 0 && d->uris) {
        *uris_out = (const char *const *)d->uris;
        return d->num_uris;
    }

    if (!d->data || d->data_length == 0)
        return 0;

    char **local = NULL;
    int n = 0, cap = 0;
    const char *p = (const char *)d->data;
    const char *end = p + d->data_length;
    while (p < end) {
        const char *nl = memchr(p, '\n', end - p);
        if (!nl) nl = end;
        size_t len = nl - p;
        if (len > 0 && p[len - 1] == '\r') len--;
        if (len > 0 && p[0] != '#') {
            if (n >= cap) {
                cap = cap ? cap * 2 : 8;
                local = realloc(local, cap * sizeof(char *));
            }
            local[n++] = strndup(p, len);
        }
        p = nl + 1;
    }
    *local_out = local;
    *uris_out = (const char *const *)local;
    return n;
}

static void free_local_uris(char **local, int n)
{
    for (int i = 0; i < n; i++)
        free(local[i]);
    free(local);
}

/* ---------- collect file paths from URIs ---------- */

static int collect_paths(const char *const *uris, int num_uris,
                         const char *target_dir, IswDndAction action,
                         char **paths_out)
{
    int npaths = 0;
    for (int i = 0; i < num_uris; i++) {
        const char *path = uris[i];
        if (strncmp(path, "file://", 7) == 0)
            path += 7;
        if (path[0] != '/')
            continue;

        const char *src_slash = strrchr(path, '/');
        if (!src_slash)
            continue;
        size_t src_dir_len = src_slash - path;
        int same_dir = (strlen(target_dir) == src_dir_len &&
                        strncmp(target_dir, path, src_dir_len) == 0);
        if (same_dir && action == ISW_DND_ACTION_MOVE)
            continue;

        paths_out[npaths++] = (char *)path;
    }
    return npaths;
}

/* ---------- drop highlight helpers ---------- */

/*
 * Translate viewport-relative coordinates to the active view widget
 * and return the item index under the pointer, or -1.
 */
static int hit_test_view(Fm *fm, int vp_x, int vp_y)
{
    Widget view;
    if (fm->view_mode == FM_VIEW_LIST)
        view = fm->listview;
    else
        view = fm->iconview;

    /* The viewport coords include the scrollbar offset and the clip
     * position.  Translate directly between X windows to get view-
     * relative coordinates regardless of widget nesting.
     * xcb_translate_coordinates works in physical pixels, so scale
     * the logical input coords and scale the result back. */
    double sf = ISWScaleFactor(fm->viewport);
    xcb_connection_t *conn = IswDisplay(fm->viewport);
    xcb_translate_coordinates_cookie_t tc =
        xcb_translate_coordinates(conn,
            IswWindow(fm->viewport), IswWindow(view),
            (int16_t)(vp_x * sf), (int16_t)(vp_y * sf));
    xcb_translate_coordinates_reply_t *tr =
        xcb_translate_coordinates_reply(conn, tc, NULL);
    if (!tr)
        return -1;
    int vx = (int)(tr->dst_x / sf + 0.5);
    int vy = (int)(tr->dst_y / sf + 0.5);
    free(tr);

    if (fm->view_mode == FM_VIEW_LIST)
        return IswListViewHitTest(view, vx, vy);
    else
        return IswIconViewHitTest(view, vx, vy);
}

static void set_drop_highlight(Fm *fm, int index)
{
    if (index == fm->dnd_drop_highlight)
        return;
    fm->dnd_drop_highlight = index;

    if (fm->view_mode == FM_VIEW_LIST)
        IswListViewSetDropHighlight(fm->listview, index);
    else
        IswIconViewSetDropHighlight(fm->iconview, index);
}

/* ---------- drag-over callbacks ---------- */

static void drag_motion_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w;
    Fm *fm = (Fm *)cd;
    IswDragOverCallbackData *d = (IswDragOverCallbackData *)call;

    int index = hit_test_view(fm, d->x, d->y);

    /* Only highlight directories — you can't drop into a regular file */
    if (index >= 0 && index < fm->nentries && fm->entries[index].is_dir)
        set_drop_highlight(fm, index);
    else
        set_drop_highlight(fm, -1);
}

static void drag_leave_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;
    set_drop_highlight(fm, -1);
}

/* ---------- content area drop callback ---------- */

static void drop_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w;
    Fm *fm = (Fm *)cd;
    IswDropCallbackData *d = (IswDropCallbackData *)call;

    char **local = NULL;
    const char *const *uris;
    int num_uris = extract_uris(d, &local, &uris);
    if (num_uris <= 0)
        return;

    /* If a directory is highlighted, drop into it; otherwise use cwd */
    const char *target_dir = fm->cwd;
    int hi = fm->dnd_drop_highlight;
    if (hi >= 0 && hi < fm->nentries && fm->entries[hi].is_dir)
        target_dir = fm->entries[hi].full_path;

    set_drop_highlight(fm, -1);

    char **paths = malloc(num_uris * sizeof(char *));
    fm->dnd_drop_was_noop = True;
    int npaths = collect_paths(uris, num_uris, target_dir, d->action, paths);
    if (npaths > 0) {
        fm->dnd_drop_was_noop = False;
        jobqueue_submit_copy(fm->app_state, fm, paths, npaths, target_dir);
    }
    free(paths);
    if (local) free_local_uris(local, num_uris);
}

/* ---------- public API ---------- */

void dnd_init(Fm *fm)
{
    fm->dnd_press_valid = False;
    fm->dnd_drop_was_noop = False;
    fm->dnd_drag_paths = NULL;
    fm->dnd_ndrag_paths = 0;
    fm->dnd_drop_highlight = -1;

    /* Register the content viewport as drop target. The places viewport
     * is registered separately in places_init(). FindDropTarget walks
     * the widget tree and finds the correct target by position. */
    xcb_atom_t uri_type = ISWXdndInternType(fm->viewport, "text/uri-list");
    ISWXdndWidgetAcceptDrops(fm->viewport);
    ISWXdndSetDropCallback(fm->viewport, drop_cb, fm);
    ISWXdndSetDragMotionCallback(fm->viewport, drag_motion_cb, fm);
    ISWXdndSetDragLeaveCallback(fm->viewport, drag_leave_cb, fm);
    ISWXdndSetAcceptedTypes(fm->viewport, &uri_type, 1);
    ISWXdndSetAcceptedActions(fm->viewport,
                              ISW_DND_ACTION_COPY | ISW_DND_ACTION_MOVE);

    /* Register drag actions and override translations on both views
     * to chain our press/motion handlers before the view's own actions.
     * IconView uses SelectItem(); ListView uses SelectRow(). */
    IswAppAddActions(fm->app_state->app, dnd_actions, IswNumber(dnd_actions));
    IswOverrideTranslations(fm->iconview, IswParseTranslationTable(
        "<Btn1Down>:   fm-dnd-press() SelectItem()\n"
        "<Btn1Motion>: fm-dnd-check() BandDrag()\n"));
    IswOverrideTranslations(fm->listview, IswParseTranslationTable(
        "<Btn1Down>:   fm-dnd-press() SelectRow()\n"
        "<Btn1Motion>: fm-dnd-check() BandDrag()\n"
        "<Btn1Up>:     BandFinish()\n"));
}

void dnd_cleanup(Fm *fm)
{
    free_drag_paths(fm);
}
