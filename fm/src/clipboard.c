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
    if (!r) return XCB_ATOM_NONE;
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
    for (int i = 0; i < clip->npaths; i++)
        free(clip->paths[i]);
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
    for (int i = 0; i < fm->clipboard.npaths; i++)
        uri_len += strlen("file://") + strlen(fm->clipboard.paths[i]) + 2;
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
        if (i < fm->clipboard.npaths - 1)
            strcat(fm->clipboard.gnome_data, "\n");
    }

    /* Own the CLIPBOARD selection via Xt so convert_selection is called */
    XtOwnSelection(fm->toplevel, fm->atom_clipboard, XCB_CURRENT_TIME,
                    convert_selection, lose_selection, NULL);
}

/* ---------- selection request handler ---------- */

/* Global FM pointer for selection callbacks */
static Fm *g_fm = NULL;

/* Called by Xt when another client requests our clipboard data */
static Boolean convert_selection(Widget w, Atom *selection, Atom *target,
                                 Atom *type_return, XtPointer *value_return,
                                 unsigned long *length_return,
                                 int *format_return)
{
    (void)w;
    (void)selection;
    Fm *fm = g_fm;
    if (!fm || fm->clipboard.npaths == 0)
        return False;

    if (*target == fm->atom_targets) {
        /* Return list of supported targets */
        static xcb_atom_t targets[3];
        targets[0] = fm->atom_targets;
        targets[1] = fm->atom_uri_list;
        targets[2] = fm->atom_gnome_files;
        *type_return = XCB_ATOM_ATOM;
        *value_return = (XtPointer)targets;
        *length_return = 3;
        *format_return = 32;
        return True;
    }

    if (*target == fm->atom_uri_list && fm->clipboard.uri_data) {
        *type_return = fm->atom_uri_list;
        *value_return = (XtPointer)fm->clipboard.uri_data;
        *length_return = strlen(fm->clipboard.uri_data);
        *format_return = 8;
        return True;
    }

    if (*target == fm->atom_gnome_files && fm->clipboard.gnome_data) {
        *type_return = fm->atom_gnome_files;
        *value_return = (XtPointer)fm->clipboard.gnome_data;
        *length_return = strlen(fm->clipboard.gnome_data);
        *format_return = 8;
        return True;
    }

    return False;
}

static void lose_selection(Widget w, Atom *selection)
{
    (void)w;
    (void)selection;
    /* Another client took the clipboard — our data is stale but
     * keep it around for internal use until replaced */
}

/* ---------- paste: request clipboard from owner ---------- */

static void do_file_op(Fm *fm, const char *src_path, FmClipOp op)
{
    const char *base = strrchr(src_path, '/');
    base = base ? base + 1 : src_path;

    size_t dlen = strlen(fm->cwd) + 1 + strlen(base) + 1;
    char *dest = malloc(dlen);
    snprintf(dest, dlen, "%s/%s", fm->cwd, base);

    if (strcmp(src_path, dest) == 0) {
        free(dest);
        return;
    }

    if (op == FM_CLIP_CUT) {
        if (rename(src_path, dest) != 0) {
            /* Cross-device: copy then delete source */
            if (fileops_copy(src_path, dest) == 0)
                fileops_delete(fm, src_path);
        }
    } else {
        fileops_copy(src_path, dest);
    }
    free(dest);
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

    if (!value || *length == 0 || *type == XCB_ATOM_NONE) {
        /* Try text/uri-list as fallback */
        if (*type == XCB_ATOM_NONE || !value) {
            XtGetSelectionValue(fm->toplevel, fm->atom_clipboard,
                                fm->atom_uri_list,
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

    if (*type == fm->atom_gnome_files) {
        if (strncmp(data, "cut\n", 4) == 0) {
            op = FM_CLIP_CUT;
            data += 4;
        } else if (strncmp(data, "copy\n", 5) == 0) {
            op = FM_CLIP_COPY;
            data += 5;
        }
    }

    /* Parse URIs and perform operations */
    char *saveptr = NULL;
    char *line = strtok_r(data, "\r\n", &saveptr);
    while (line) {
        const char *path = line;
        if (strncmp(path, "file://", 7) == 0)
            path += 7;
        if (path[0] == '/')
            do_file_op(fm, path, op);
        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    free(buf);
    fm_refresh(fm);
}

/* ---------- public API ---------- */

void clipboard_init(Fm *fm)
{
    g_fm = fm;
    xcb_connection_t *conn = XtDisplay(fm->toplevel);
    fm->atom_clipboard   = intern(conn, "CLIPBOARD");
    fm->atom_targets     = intern(conn, "TARGETS");
    fm->atom_uri_list    = intern(conn, "text/uri-list");
    fm->atom_gnome_files = intern(conn, "x-special/gnome-copied-files");
    fm->atom_utf8_string = intern(conn, "UTF8_STRING");
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
    /* First try gnome-copied-files (has cut/copy info),
     * fall back to text/uri-list */
    XtGetSelectionValue(fm->toplevel, fm->atom_clipboard,
                        fm->atom_gnome_files,
                        receive_paste, fm, XCB_CURRENT_TIME);
}

void clipboard_cleanup(Fm *fm)
{
    clip_free(&fm->clipboard);
}
