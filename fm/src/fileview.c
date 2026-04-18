#define _POSIX_C_SOURCE 200809L
/*
 * fileview.c — populate IconView or ListView from browser entries
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xcb/xcb.h>

/* ---------- double-click tracking ---------- */

static int is_double_click(Fm *fm, int index)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long ms = (now.tv_sec - fm->last_click_time.tv_sec) * 1000 +
              (now.tv_nsec - fm->last_click_time.tv_nsec) / 1000000;

    int dbl = (index == fm->last_click_index &&
               ms < isde_config_double_click_ms());

    fm->last_click_index = index;
    fm->last_click_time = now;

    return dbl;
}

/* ---------- sort helpers ---------- */

static Fm *sort_fm;  /* qsort context — not reentrant but fine for single-thread */

static int cmp_name_asc(const void *a, const void *b)
{
    const FmEntry *ea = (const FmEntry *)a;
    const FmEntry *eb = (const FmEntry *)b;
    return strcasecmp(ea->name, eb->name);
}

static int cmp_name_desc(const void *a, const void *b)
{
    const FmEntry *ea = (const FmEntry *)a;
    const FmEntry *eb = (const FmEntry *)b;
    return strcasecmp(eb->name, ea->name);
}

static const char *entry_type_str(const FmEntry *e)
{
    if (e->is_dir) return "Folder";
    const char *dot = strrchr(e->name, '.');
    if (dot && dot[1]) return dot + 1;
    if (e->mode & S_IXUSR) return "Executable";
    return "File";
}

static int cmp_type_asc(const void *a, const void *b)
{
    const FmEntry *ea = (const FmEntry *)a;
    const FmEntry *eb = (const FmEntry *)b;
    int r = strcasecmp(entry_type_str(ea), entry_type_str(eb));
    if (r != 0) return r;
    return strcasecmp(ea->name, eb->name);
}

static int cmp_type_desc(const void *a, const void *b)
{
    const FmEntry *ea = (const FmEntry *)a;
    const FmEntry *eb = (const FmEntry *)b;
    int r = strcasecmp(entry_type_str(eb), entry_type_str(ea));
    if (r != 0) return r;
    return strcasecmp(ea->name, eb->name);
}

static int cmp_size_asc(const void *a, const void *b)
{
    const FmEntry *ea = (const FmEntry *)a;
    const FmEntry *eb = (const FmEntry *)b;
    if (ea->size < eb->size) return -1;
    if (ea->size > eb->size) return 1;
    return strcasecmp(ea->name, eb->name);
}

static int cmp_size_desc(const void *a, const void *b)
{
    const FmEntry *ea = (const FmEntry *)a;
    const FmEntry *eb = (const FmEntry *)b;
    if (ea->size > eb->size) return -1;
    if (ea->size < eb->size) return 1;
    return strcasecmp(ea->name, eb->name);
}

static void sort_entries(Fm *fm)
{
    typedef int (*CmpFn)(const void *, const void *);
    static const CmpFn cmps[3][2] = {
        { cmp_name_asc, cmp_name_desc },
        { cmp_type_asc, cmp_type_desc },
        { cmp_size_asc, cmp_size_desc },
    };

    int col = fm->sort_col;
    int dir = (fm->sort_dir == IswListViewSortDescending) ? 1 : 0;

    if (col >= 0 && col < 3)
        qsort(fm->entries, fm->nentries, sizeof(FmEntry), cmps[col][dir]);
}

/* Format file size for display */
static char *format_size(off_t size)
{
    char buf[32];
    if (size < 1024)
        snprintf(buf, sizeof(buf), "%ld B", (long)size);
    else if (size < 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f KB", size / 1024.0);
    else if (size < 1024L * 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f MB", size / (1024.0 * 1024));
    else
        snprintf(buf, sizeof(buf), "%.1f GB", size / (1024.0 * 1024 * 1024));
    return strdup(buf);
}

/* ---------- IconView callback ---------- */

static void iconview_callback(Widget w, IswPointer client_data,
                              IswPointer call_data)
{
    Fm *fm = (Fm *)client_data;
    IswIconViewCallbackData *d = (IswIconViewCallbackData *)call_data;

    fm_dismiss_context(fm);

    /* Check if triggered by keyboard (Enter/Return) — always open */
    xcb_generic_event_t *ev = IswLastEventProcessed(IswDisplay(w));
    if (ev) {
        uint8_t type = ev->response_type & ~0x80;
        if (type == XCB_KEY_PRESS || type == XCB_KEY_RELEASE) {
            browser_open_entry(fm, d->index);
            return;
        }
        /* Ignore callbacks fired from ButtonRelease (BandFinish deselect);
         * only ButtonPress should count for double-click detection */
        if (type == XCB_BUTTON_RELEASE)
            return;
    }

    if (fm->double_click) {
        if (is_double_click(fm, d->index))
            browser_open_entry(fm, d->index);
    } else {
        browser_open_entry(fm, d->index);
    }
}

/* ---------- ListView callbacks ---------- */

static void listview_callback(Widget w, IswPointer client_data,
                              IswPointer call_data)
{
    Fm *fm = (Fm *)client_data;
    IswListViewCallbackData *d = (IswListViewCallbackData *)call_data;

    fm_dismiss_context(fm);

    if (d->row < 0 || d->row >= fm->nentries)
        return;

    xcb_generic_event_t *ev = IswLastEventProcessed(IswDisplay(w));
    if (ev) {
        uint8_t type = ev->response_type & ~0x80;
        if (type == XCB_KEY_PRESS || type == XCB_KEY_RELEASE) {
            browser_open_entry(fm, d->row);
            return;
        }
        if (type == XCB_BUTTON_RELEASE)
            return;
    }

    if (fm->double_click) {
        if (is_double_click(fm, d->row))
            browser_open_entry(fm, d->row);
    } else {
        browser_open_entry(fm, d->row);
    }
}

static void reorder_callback(Widget w, IswPointer client_data,
                             IswPointer call_data)
{
    (void)w;
    Fm *fm = (Fm *)client_data;
    IswListViewReorderCallbackData *d =
        (IswListViewReorderCallbackData *)call_data;

    /* Map ListView column to sort column */
    FmSortColumn col;
    switch (d->column) {
    case 0: col = FM_SORT_NAME; break;
    case 1: col = FM_SORT_TYPE; break;
    case 2: col = FM_SORT_SIZE; break;
    default: return;
    }

    fm->sort_col = col;
    fm->sort_dir = d->direction;

    /* Re-sort and repopulate */
    sort_entries(fm);
    fileview_populate(fm);

    /* Update sort indicator */
    IswListViewSetSort(fm->listview, d->column, d->direction);
}

/* ---------- selection abstraction ---------- */

int fileview_get_selected(Fm *fm)
{
    if (fm->view_mode == FM_VIEW_LIST && fm->listview)
        return IswListViewGetSelected(fm->listview);
    if (fm->iconview)
        return IswIconViewGetSelected(fm->iconview);
    return -1;
}

int fileview_get_selected_items(Fm *fm, int **indices_out)
{
    if (fm->view_mode == FM_VIEW_LIST && fm->listview)
        return IswListViewGetSelectedRows(fm->listview, indices_out);
    if (fm->iconview)
        return IswIconViewGetSelectedItems(fm->iconview, indices_out);
    *indices_out = NULL;
    return 0;
}

/* ---------- ListView data helpers ---------- */

#define LV_NCOLS 3

static void lv_free_data(Fm *fm)
{
    for (int i = 0; i < fm->lv_nrows; i++)
        free(fm->lv_size_strs[i]);
    free(fm->lv_size_strs);
    free(fm->lv_data);
    fm->lv_data = NULL;
    fm->lv_size_strs = NULL;
    fm->lv_nrows = 0;
}

static void lv_build_data(Fm *fm)
{
    lv_free_data(fm);

    int n = fm->nentries;
    fm->lv_nrows = n;
    fm->lv_data = malloc(n * LV_NCOLS * sizeof(String));
    fm->lv_size_strs = malloc(n * sizeof(char *));

    for (int i = 0; i < n; i++) {
        FmEntry *e = &fm->entries[i];
        fm->lv_data[i * LV_NCOLS + 0] = (String)e->name;
        fm->lv_data[i * LV_NCOLS + 1] = (String)entry_type_str(e);
        fm->lv_size_strs[i] = e->is_dir ? strdup("--") : format_size(e->size);
        fm->lv_data[i * LV_NCOLS + 2] = fm->lv_size_strs[i];
    }
}

/* ---------- init / populate ---------- */

static IswListViewColumn lv_columns[] = {
    { "Name", 200, 80 },
    { "Type", 100, 50 },
    { "Size",  80, 50 },
};

void fileview_init(Fm *fm)
{
    /* Viewport for scrolling — below the nav bar, right of places sidebar */
    Arg args[20];
    Cardinal n = 0;
    IswSetArg(args[n], IswNallowVert, True);          n++;
    IswSetArg(args[n], IswNallowHoriz, False);        n++;
    IswSetArg(args[n], IswNuseRight, True);            n++;
    IswSetArg(args[n], IswNborderWidth, 0);            n++;
    IswSetArg(args[n], IswNflexGrow, 1);               n++;
    fm->viewport = IswCreateManagedWidget("viewport", viewportWidgetClass,
                                         fm->hbox, args, n);

    /* Default sort: name descending */
    fm->sort_col = FM_SORT_NAME;
    fm->sort_dir = IswListViewSortDescending;

    /* IconView inside viewport */
    n = 0;
    IswSetArg(args[n], IswNborderWidth, 0);     n++;
    IswSetArg(args[n], IswNiconSize, 64);        n++;
    IswSetArg(args[n], IswNitemSpacing, 16);     n++;
    IswSetArg(args[n], IswNmultiSelect, True);   n++;
    IswSetArg(args[n], "labelLines", 3);         n++;
    fm->iconview = IswCreateManagedWidget("iconView", iconViewWidgetClass,
                                         fm->viewport, args, n);
    IswAddCallback(fm->iconview, IswNselectCallback, iconview_callback, fm);
    fm_register_context_menu(fm, fm->iconview);
    fm_install_shortcuts(fm->iconview);

    /* ListView inside viewport (initially unmanaged) */
    n = 0;
    IswSetArg(args[n], IswNborderWidth, 0);                     n++;
    IswSetArg(args[n], IswNlistViewColumns, lv_columns);        n++;
    IswSetArg(args[n], IswNnumColumns, LV_NCOLS);               n++;
    IswSetArg(args[n], IswNmultiSelect, True);                   n++;
    IswSetArg(args[n], IswNshowHeader, True);                    n++;
    fm->listview = IswCreateWidget("listView", listViewWidgetClass,
                                  fm->viewport, args, n);
    IswAddCallback(fm->listview, IswNselectCallback, listview_callback, fm);
    IswAddCallback(fm->listview, IswNreorderCallback, reorder_callback, fm);
    fm_register_context_menu(fm, fm->listview);
    fm_install_shortcuts(fm->listview);

    /* Default to icon view */
    fm->view_mode = FM_VIEW_ICON;

    /* Status bar */
    Widget statusbar = IswMainWindowGetStatusBar(fm->main_window);
    if (statusbar) {
        n = 0;
        IswSetArg(args[n], IswNlabel, "");           n++;
        IswSetArg(args[n], IswNborderWidth, 0);      n++;
        IswSetArg(args[n], IswNstatusStretch, True);  n++;
        fm->status_label = IswCreateManagedWidget("status", labelWidgetClass,
                                                  statusbar, args, n);
    }
}

void fileview_set_mode(Fm *fm, FmViewMode mode)
{
    if (fm->view_mode == mode)
        return;

    fm->view_mode = mode;

    if (mode == FM_VIEW_ICON) {
        IswUnmanageChild(fm->listview);
        IswManageChild(fm->iconview);
    } else {
        IswUnmanageChild(fm->iconview);
        IswManageChild(fm->listview);
    }

    fileview_populate(fm);
}

void fileview_populate(Fm *fm)
{
    if (fm->view_mode == FM_VIEW_ICON && fm->iconview) {
        free(fm->fv_labels);
        free(fm->fv_icons);

        fm->fv_labels = malloc(fm->nentries * sizeof(String));
        fm->fv_icons  = malloc(fm->nentries * sizeof(String));

        for (int i = 0; i < fm->nentries; i++) {
            fm->fv_labels[i] = (String)fm->entries[i].name;
            fm->fv_icons[i]  = (String)fm->entries[i].mime_icon;
        }
        IswIconViewSetItems(fm->iconview, fm->fv_labels, fm->fv_icons,
                            fm->nentries);
    }

    if (fm->view_mode == FM_VIEW_LIST && fm->listview) {
        sort_entries(fm);
        lv_build_data(fm);
        IswListViewSetData(fm->listview, fm->lv_data, fm->lv_nrows, LV_NCOLS);

        /* Restore sort indicator */
        int vis_col = (int)fm->sort_col;  /* enum matches column index */
        IswListViewSetSort(fm->listview, vis_col, fm->sort_dir);
    }

    /* Update status bar */
    if (fm->status_label) {
        char buf[128];
        int ndirs = 0, nfiles = 0;
        for (int i = 0; i < fm->nentries; i++) {
            if (fm->entries[i].is_dir) {
                ndirs++;
            } else {
                nfiles++;
            }
        }
        snprintf(buf, sizeof(buf), "%d folders, %d files", ndirs, nfiles);
        Arg args[20];
        IswSetArg(args[0], IswNlabel, buf);
        IswSetValues(fm->status_label, args, 1);
    }
}

void fileview_cleanup(Fm *fm)
{
    free(fm->fv_labels);
    free(fm->fv_icons);
    fm->fv_labels = NULL;
    fm->fv_icons = NULL;

    lv_free_data(fm);
}
