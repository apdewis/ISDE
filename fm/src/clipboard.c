#define _POSIX_C_SOURCE 200809L
/*
 * clipboard.c — X11 CLIPBOARD selection for file copy/cut/paste
 *
 * Uses text/uri-list and x-special/gnome-copied-files targets
 * for compatibility with other file managers.
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- helpers ---------- */

static xcb_atom_t intern(xcb_connection_t *c, const char *name)
{
    xcb_intern_atom_cookie_t ck = xcb_intern_atom(c, 0, strlen(name), name);
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, ck, NULL);
    if (!r) { return XCB_ATOM_NONE; }
    xcb_atom_t a = r->atom;
    free(r);
    return a;
}

/* Forward declarations for selection callbacks */
static Boolean convert_selection(Widget, Atom *, Atom *, Atom *,
                                 XtPointer *, unsigned long *, int *);
static void lose_selection(Widget, Atom *);

static void clip_free(FmClipboard *clip)
{
    for (int i = 0; i < clip->npaths; i++) {
        free(clip->paths[i]);
    }
    free(clip->paths);
    free(clip->uri_data);
    free(clip->gnome_data);
    memset(clip, 0, sizeof(*clip));
}

/* Build the selected file paths into the clipboard */
static void clip_set_from_selection(Fm *fm, FmClipOp op)
{
    clip_free(&fm->clipboard);
    fm->clipboard.op = op;

    /* Collect selected entries */
    int cap = 16;
    fm->clipboard.paths = malloc(cap * sizeof(char *));
    fm->clipboard.npaths = 0;

    /* Get selected indices from IconView */
    if (fm->iconview) {
        int *indices = NULL;
        int nsel = IswIconViewGetSelectedItems(fm->iconview, &indices);
        for (int i = 0; i < nsel; i++) {
            int idx = indices[i];
            if (idx >= 0 && idx < fm->nentries) {
                if (fm->clipboard.npaths >= cap) {
                    cap *= 2;
                    fm->clipboard.paths = realloc(fm->clipboard.paths,
                                                   cap * sizeof(char *));
                }
                fm->clipboard.paths[fm->clipboard.npaths++] =
                    strdup(fm->entries[idx].full_path);
            }
        }
        free(indices);
    }

    if (fm->clipboard.npaths == 0) {
        clip_free(&fm->clipboard);
        return;
    }

    /* Build text/uri-list */
    size_t uri_len = 0;
    for (int i = 0; i < fm->clipboard.npaths; i++) {
        uri_len += strlen("file://") + strlen(fm->clipboard.paths[i]) + 2;
    }
    fm->clipboard.uri_data = malloc(uri_len + 1);
    fm->clipboard.uri_data[0] = '\0';
    for (int i = 0; i < fm->clipboard.npaths; i++) {
        strcat(fm->clipboard.uri_data, "file://");
        strcat(fm->clipboard.uri_data, fm->clipboard.paths[i]);
        strcat(fm->clipboard.uri_data, "\r\n");
    }

    /* Build x-special/gnome-copied-files */
    const char *op_str = (op == FM_CLIP_CUT) ? "cut" : "copy";
    size_t gnome_len = strlen(op_str) + 1 + uri_len;
    fm->clipboard.gnome_data = malloc(gnome_len + 1);
    strcpy(fm->clipboard.gnome_data, op_str);
    strcat(fm->clipboard.gnome_data, "\n");
    for (int i = 0; i < fm->clipboard.npaths; i++) {
        strcat(fm->clipboard.gnome_data, "file://");
        strcat(fm->clipboard.gnome_data, fm->clipboard.paths[i]);
        if (i < fm->clipboard.npaths - 1) {
            strcat(fm->clipboard.gnome_data, "\n");
        }
    }

    /* Own the CLIPBOARD selection via Xt so convert_selection is called */
    FmApp *app = fm->app_state;
    XtOwnSelection(fm->toplevel, app->atom_clipboard, XCB_CURRENT_TIME,
                    convert_selection, lose_selection, NULL);
    app->clipboard_owner = fm;
}

/* ---------- selection request handler ---------- */

/* Called by Xt when another client requests our clipboard data */
static Boolean convert_selection(Widget w, Atom *selection, Atom *target,
                                 Atom *type_return, XtPointer *value_return,
                                 unsigned long *length_return,
                                 int *format_return)
{
    (void)selection;
    Fm *fm = fm_from_widget(w);
    if (!fm || fm->clipboard.npaths == 0)
        return False;

    FmApp *app = fm->app_state;

    if (*target == app->atom_targets) {
        /* Return list of supported targets */
        static xcb_atom_t targets[3];
        targets[0] = app->atom_targets;
        targets[1] = app->atom_uri_list;
        targets[2] = app->atom_gnome_files;
        *type_return = XCB_ATOM_ATOM;
        *value_return = (XtPointer)targets;
        *length_return = 3;
        *format_return = 32;
        return True;
    }

    if (*target == app->atom_uri_list && fm->clipboard.uri_data) {
        *type_return = app->atom_uri_list;
        *value_return = (XtPointer)fm->clipboard.uri_data;
        *length_return = strlen(fm->clipboard.uri_data);
        *format_return = 8;
        return True;
    }

    if (*target == app->atom_gnome_files && fm->clipboard.gnome_data) {
        *type_return = app->atom_gnome_files;
        *value_return = (XtPointer)fm->clipboard.gnome_data;
        *length_return = strlen(fm->clipboard.gnome_data);
        *format_return = 8;
        return True;
    }

    return False;
}

static void lose_selection(Widget w, Atom *selection)
{
    (void)selection;
    Fm *fm = fm_from_widget(w);
    if (fm) {
        FmApp *app = fm->app_state;
        if (app->clipboard_owner == fm) {
            app->clipboard_owner = NULL;
        }
    }
}

/* ---------- paste: submit file operations ---------- */

static void submit_paste_job(Fm *fm, char **paths, int npaths, FmClipOp op)
{
    if (npaths <= 0) { return; }
    FmApp *app = fm->app_state;
    if (op == FM_CLIP_CUT) {
        jobqueue_submit_move(app, fm, paths, npaths, fm->cwd);
    } else {
        jobqueue_submit_copy(app, fm, paths, npaths, fm->cwd);
    }
}

static void receive_paste(Widget w, XtPointer client_data,
                          Atom *selection, Atom *type,
                          XtPointer value, unsigned long *length,
                          int *format)
{
    (void)w;
    (void)selection;
    (void)format;
    Fm *fm = (Fm *)client_data;
    FmApp *app = fm->app_state;

    if (!value || *length == 0 || *type == XCB_ATOM_NONE) {
        /* Try text/uri-list as fallback */
        if (*type == XCB_ATOM_NONE || !value) {
            XtGetSelectionValue(fm->toplevel, app->atom_clipboard,
                                app->atom_uri_list,
                                receive_paste, fm, XCB_CURRENT_TIME);
        }
        return;
    }

    /* Work on a copy so strtok doesn't corrupt the clipboard owner's data */
    char *buf = malloc(*length + 1);
    memcpy(buf, value, *length);
    buf[*length] = '\0';

    char *data = buf;
    FmClipOp op = FM_CLIP_COPY;

    if (*type == app->atom_gnome_files) {
        if (strncmp(data, "cut\n", 4) == 0) {
            op = FM_CLIP_CUT;
            data += 4;
        } else if (strncmp(data, "copy\n", 5) == 0) {
            op = FM_CLIP_COPY;
            data += 5;
        }
    }

    /* Parse URIs and collect paths */
    int cap = 16, npaths = 0;
    char **paths = malloc(cap * sizeof(char *));

    char *saveptr = NULL;
    char *line = strtok_r(data, "\r\n", &saveptr);
    while (line) {
        const char *path = line;
        if (strncmp(path, "file://", 7) == 0) {
            path += 7;
        }
        if (path[0] == '/') {
            if (npaths >= cap) {
                cap *= 2;
                paths = realloc(paths, cap * sizeof(char *));
            }
            paths[npaths++] = (char *)path;
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    submit_paste_job(fm, paths, npaths, op);
    free(paths);
    free(buf);
}

/* ---------- public API ---------- */

void clipboard_init(Fm *fm)
{
    xcb_connection_t *conn = XtDisplay(fm->toplevel);
    FmApp *app = fm->app_state;
    app->atom_clipboard   = intern(conn, "CLIPBOARD");
    app->atom_targets     = intern(conn, "TARGETS");
    app->atom_uri_list    = intern(conn, "text/uri-list");
    app->atom_gnome_files = intern(conn, "x-special/gnome-copied-files");
    app->atom_utf8_string = intern(conn, "UTF8_STRING");

    /* Fm* is stored on the toplevel via fm_set_context in fm_init,
     * recovered in convert_selection via fm_from_widget. */
}

void clipboard_copy(Fm *fm)
{
    clip_set_from_selection(fm, FM_CLIP_COPY);
}

void clipboard_cut(Fm *fm)
{
    clip_set_from_selection(fm, FM_CLIP_CUT);
}

void clipboard_paste(Fm *fm)
{
    FmApp *app = fm->app_state;

    /* Fast path: if THIS window owns the clipboard, use local data
     * directly instead of round-tripping through X selections.
     * Another window in the same process goes through the selection. */
    if (app->clipboard_owner == fm && fm->clipboard.npaths > 0) {
        submit_paste_job(fm, fm->clipboard.paths, fm->clipboard.npaths,
                         fm->clipboard.op);
        return;
    }

    /* Otherwise request from external clipboard owner */
    XtGetSelectionValue(fm->toplevel, app->atom_clipboard,
                        app->atom_gnome_files,
                        receive_paste, fm, XCB_CURRENT_TIME);
}

void clipboard_cleanup(Fm *fm)
{
    clip_free(&fm->clipboard);
}
