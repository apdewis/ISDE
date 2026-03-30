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
                            XtPointer *data_return,
                            unsigned long *length_return,
                            int *format_return,
                            XtPointer client_data)
{
    (void)w;
    Fm *fm = (Fm *)client_data;
    xcb_atom_t uri_atom = ISWXdndInternType(fm->iconview, "text/uri-list");

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

    char *buf = XtMalloc(total + 1);
    char *p = buf;
    for (int i = 0; i < fm->dnd_ndrag_paths; i++) {
        p += sprintf(p, "file://%s\r\n", fm->dnd_drag_paths[i]);
    }

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
    int nsel = IswIconViewGetSelectedItems(fm->iconview, &indices);
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
    uri_type = ISWXdndInternType(fm->iconview, "text/uri-list");

    IswDragSourceDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.types       = &uri_type;
    desc.num_types   = 1;
    desc.actions     = ISW_DND_ACTION_COPY | ISW_DND_ACTION_MOVE;
    desc.convert     = drag_convert;
    desc.finished    = drag_finished;
    desc.client_data = fm;

    ISWXdndStartDrag(fm->iconview, &fm->dnd_saved_press, &desc);
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
    int sel = IswIconViewGetSelected(fm->iconview);
    if (sel < 0) {
        return;
    }

    /* Use the motion event's timestamp for the grab — Xephyr rejects
     * the original press timestamp because it matches the passive
     * grab's activation time (not strictly "later than"). */
    fm->dnd_saved_press.time = motion->time;

    fm->dnd_press_valid = False;
    start_drag(fm);
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

    if (d->num_uris <= 0 || !d->uris) {
        return;
    }

    const char *target_dir = fm->cwd;

    /* Collect valid file paths */
    int cap = d->num_uris;
    char **paths = malloc(cap * sizeof(char *));
    int npaths = 0;

    fm->dnd_drop_was_noop = True;

    for (int i = 0; i < d->num_uris; i++) {
        const char *uri = d->uris[i];
        const char *path = uri;
        if (strncmp(path, "file://", 7) == 0) {
            path += 7;
        }
        if (path[0] != '/') {
            continue;
        }

        const char *src_slash = strrchr(path, '/');
        if (!src_slash) {
            continue;
        }
        size_t src_dir_len = src_slash - path;
        int same_dir = (strlen(target_dir) == src_dir_len &&
                        strncmp(target_dir, path, src_dir_len) == 0);

        if (same_dir && d->action == ISW_DND_ACTION_MOVE) {
            continue;
        }

        paths[npaths++] = (char *)path;
        fm->dnd_drop_was_noop = False;
    }

    if (npaths > 0) {
        jobqueue_submit_copy(fm->app_state, fm, paths, npaths, target_dir);
    }
    free(paths);
}

/* ---------- public API ---------- */

void dnd_init(Fm *fm)
{
    fm->dnd_press_valid = False;
    fm->dnd_drop_was_noop = False;
    fm->dnd_drag_paths = NULL;
    fm->dnd_ndrag_paths = 0;

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
    XtAppAddActions(fm->app_state->app, dnd_actions, XtNumber(dnd_actions));
    XtOverrideTranslations(fm->iconview, XtParseTranslationTable(
        "<Btn1Down>:   fm-dnd-press() SelectItem()\n"
        "<Btn1Motion>: fm-dnd-check() BandDrag()\n"));
}

void dnd_cleanup(Fm *fm)
{
    free_drag_paths(fm);
}
