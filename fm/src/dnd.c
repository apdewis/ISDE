#define _POSIX_C_SOURCE 200809L
/*
 * dnd.c — XDND drag-and-drop for the file manager
 *
 * Registers the IconView as both a drag source and drop target.
 * Drag: selected files can be dragged to other windows/apps.
 * Drop: files from other windows/apps can be dropped into the
 *        current directory.
 *
 * Drag initiation uses Xt translation overrides that chain with the
 * IconView's own SelectItem/BandDrag actions.
 *
 * Uses standard XDND v5 move semantics: the drop target always
 * copies; the drag source deletes originals on move completion.
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <xcb/xcb.h>

#define DND_THRESHOLD 5  /* pixels of motion before drag starts */

/* ---------- drag state ---------- */

static Fm     *drag_fm;
static xcb_button_press_event_t saved_press;
static Boolean press_valid;

/* Paths of files being dragged (for convert + finished callbacks) */
static char  **drag_paths;
static int     ndrag_paths;

/* Set by drop_cb when all URIs were skipped (same-dir move) */
static Boolean drop_was_noop;

/* ---------- drag path helpers ---------- */

static void free_drag_paths(void)
{
    for (int i = 0; i < ndrag_paths; i++)
        free(drag_paths[i]);
    free(drag_paths);
    drag_paths = NULL;
    ndrag_paths = 0;
}

/* ---------- drag source: convert callback ---------- */

static Boolean drag_convert(Widget w, xcb_atom_t target_type,
                            XtPointer *data_return,
                            unsigned long *length_return,
                            int *format_return,
                            XtPointer client_data)
{
    (void)w;
    Fm *fm = (Fm *)client_data;
    xcb_atom_t uri_atom = ISWXdndInternType(fm->iconview, "text/uri-list");

    fprintf(stderr, "DND convert: target_type=%u uri_atom=%u ndrag=%d\n",
            target_type, uri_atom, ndrag_paths);
    if (target_type != uri_atom)
        return False;

    if (ndrag_paths <= 0)
        return False;

    size_t total = 0;
    for (int i = 0; i < ndrag_paths; i++)
        total += 7 + strlen(drag_paths[i]) + 2;

    char *buf = XtMalloc(total + 1);
    char *p = buf;
    for (int i = 0; i < ndrag_paths; i++)
        p += sprintf(p, "file://%s\r\n", drag_paths[i]);

    *data_return = (XtPointer)buf;
    *length_return = p - buf;
    *format_return = 8;
    return True;
}

/* ---------- drag source: finished callback ---------- */

static void drag_finished(Widget w, IswDndAction action,
                          Boolean accepted, XtPointer client_data)
{
    (void)w;
    Fm *fm = (Fm *)client_data;
    fprintf(stderr, "DND finished: action=%d accepted=%d noop=%d\n",
            action, accepted, drop_was_noop);

    if (accepted && action == ISW_DND_ACTION_MOVE &&
        !drop_was_noop && drag_paths) {
        for (int i = 0; i < ndrag_paths; i++) {
            struct stat st;
            if (lstat(drag_paths[i], &st) == 0)
                fileops_delete(fm, drag_paths[i]);
        }
        fm_refresh(fm);
    }

    free_drag_paths();
}

/* ---------- drag source: initiate ---------- */

static void start_drag(Fm *fm)
{
    free_drag_paths();
    drop_was_noop = False;

    /* Snapshot selected file paths for convert + finished callbacks.
     * By this point SelectItem has already run on the Btn1Down event,
     * so the selection reflects the user's click. */
    int *indices = NULL;
    int nsel = IswIconViewGetSelectedItems(fm->iconview, &indices);
    if (nsel <= 0) {
        free(indices);
        return;
    }

    drag_paths = malloc(nsel * sizeof(char *));
    for (int i = 0; i < nsel; i++) {
        int idx = indices[i];
        if (idx >= 0 && idx < fm->nentries)
            drag_paths[ndrag_paths++] = strdup(fm->entries[idx].full_path);
    }
    free(indices);

    if (ndrag_paths <= 0) {
        free_drag_paths();
        return;
    }

    static xcb_atom_t uri_type;
    uri_type = ISWXdndInternType(fm->iconview, "text/uri-list");

    IswDragSourceDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.types       = &uri_type;
    desc.num_types   = 1;
    desc.actions     = ISW_DND_ACTION_COPY | ISW_DND_ACTION_MOVE;
    desc.convert     = drag_convert;
    desc.finished    = drag_finished;
    desc.client_data = fm;

    fprintf(stderr, "DND start_drag: %d paths, calling ISWXdndStartDrag\n", ndrag_paths);
    ISWXdndStartDrag(fm->iconview, &saved_press, &desc);
    fprintf(stderr, "DND start_drag: returned\n");
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
    (void)w; (void)params; (void)num_params;

    uint8_t type = ev->response_type & ~0x80;
    if (type != XCB_BUTTON_PRESS) {
        press_valid = False;
        return;
    }

    xcb_button_press_event_t *bp = (xcb_button_press_event_t *)ev;
    if (bp->detail != 1) {
        press_valid = False;
        return;
    }

    memcpy(&saved_press, bp, sizeof(saved_press));
    press_valid = True;
    fprintf(stderr, "DND press: btn=%d x=%d y=%d time=%u\n",
            bp->detail, bp->event_x, bp->event_y, bp->time);
}

static void act_dnd_check(Widget w, xcb_generic_event_t *ev,
                          String *params, Cardinal *num_params)
{
    (void)w; (void)params; (void)num_params;

    if (!press_valid)
        return;

    uint8_t type = ev->response_type & ~0x80;
    if (type != XCB_MOTION_NOTIFY)
        return;

    xcb_motion_notify_event_t *motion = (xcb_motion_notify_event_t *)ev;
    int dx = motion->event_x - saved_press.event_x;
    int dy = motion->event_y - saved_press.event_y;
    if (dx * dx + dy * dy < DND_THRESHOLD * DND_THRESHOLD)
        return;

    /* Only start drag if something is selected (not rubber band) */
    int sel = IswIconViewGetSelected(drag_fm->iconview);
    fprintf(stderr, "DND check: dist_sq=%d sel=%d\n", dx*dx + dy*dy, sel);
    if (sel < 0)
        return;

    /* Use the motion event's timestamp for the grab — Xephyr rejects
     * the original press timestamp because it matches the passive
     * grab's activation time (not strictly "later than"). */
    saved_press.time = motion->time;

    press_valid = False;
    fprintf(stderr, "DND check: starting drag, time=%u\n", saved_press.time);
    start_drag(drag_fm);
}

static XtActionsRec dnd_actions[] = {
    {"fm-dnd-press", act_dnd_press},
    {"fm-dnd-check", act_dnd_check},
};

/* ---------- drop target callback ---------- */

static void drop_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w;
    Fm *fm = (Fm *)cd;
    IswDropCallbackData *d = (IswDropCallbackData *)call;

    fprintf(stderr, "DND drop_cb: num_uris=%d action=%d x=%d y=%d\n",
            d->num_uris, d->action, d->x, d->y);
    if (d->num_uris <= 0 || !d->uris)
        return;
    for (int i = 0; i < d->num_uris; i++)
        fprintf(stderr, "  uri[%d]: %s\n", i, d->uris[i]);

    const char *target_dir = fm->cwd;

    drop_was_noop = True;

    for (int i = 0; i < d->num_uris; i++) {
        const char *uri = d->uris[i];
        const char *path = uri;
        if (strncmp(path, "file://", 7) == 0)
            path += 7;
        if (path[0] != '/')
            continue;

        /* Check if source and target are in the same directory */
        const char *src_slash = strrchr(path, '/');
        if (!src_slash)
            continue;
        size_t src_dir_len = src_slash - path;
        int same_dir = (strlen(target_dir) == src_dir_len &&
                        strncmp(target_dir, path, src_dir_len) == 0);

        /* Same-dir move is a no-op */
        if (same_dir && d->action == ISW_DND_ACTION_MOVE)
            continue;

        const char *base = src_slash + 1;
        size_t dlen = strlen(target_dir) + 1 + strlen(base) + 1;
        char *dest = malloc(dlen);
        snprintf(dest, dlen, "%s/%s", target_dir, base);

        fileops_copy(path, dest);
        free(dest);
        drop_was_noop = False;
    }

    fm_refresh(fm);
}

/* ---------- public API ---------- */

void dnd_init(Fm *fm)
{
    drag_fm = fm;
    press_valid = False;
    drop_was_noop = False;

    /* Register the shell as drop target.  The Viewport's clip window
     * is not a Composite, so FindDropChild can't traverse past it
     * to reach the iconview.  Registering on the shell works because
     * FindDropTarget checks it first via DropConfig. */
    xcb_atom_t uri_type = ISWXdndInternType(fm->toplevel, "text/uri-list");
    ISWXdndWidgetAcceptDrops(fm->toplevel);
    ISWXdndSetDropCallback(fm->toplevel, drop_cb, fm);
    ISWXdndSetAcceptedTypes(fm->toplevel, &uri_type, 1);
    ISWXdndSetAcceptedActions(fm->toplevel,
                              ISW_DND_ACTION_COPY | ISW_DND_ACTION_MOVE);

    /* Register drag actions and override IconView translations to
     * chain our press/motion handlers before SelectItem/BandDrag. */
    XtAppAddActions(fm->app, dnd_actions, XtNumber(dnd_actions));
    XtOverrideTranslations(fm->iconview, XtParseTranslationTable(
        "<Btn1Down>:   fm-dnd-press() SelectItem()\n"
        "<Btn1Motion>: fm-dnd-check() BandDrag()\n"));
}

void dnd_cleanup(Fm *fm)
{
    (void)fm;
    free_drag_paths();
}
