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
#include <ISW/IswArgMacros.h>

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
    if (fm->ctx_target_index >= 0)
        return fm->ctx_target_index;
    if (fm->view_mode == FM_VIEW_LIST && fm->listview)
        return IswListViewGetSelected(fm->listview);
    if (fm->iconview)
        return IswIconViewGetSelected(fm->iconview);
    return -1;
}

int fileview_get_selected_items(Fm *fm, int **indices_out)
{
    if (fm->ctx_target_index >= 0) {
        int *out = malloc(sizeof(int));
        if (!out) { *indices_out = NULL; return 0; }
        out[0] = fm->ctx_target_index;
        *indices_out = out;
        return 1;
    }
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
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgAllowVert(&ab, True);
    IswArgAllowHoriz(&ab, False);
    IswArgUseRight(&ab, True);
    IswArgBorderWidth(&ab, 0);
    IswArgFlexGrow(&ab, 1);
    fm->viewport = IswCreateManagedWidget("viewport", viewportWidgetClass,
                                         fm->hbox, ab.args, ab.count);

    /* Default sort: name descending */
    fm->sort_col = FM_SORT_NAME;
    fm->sort_dir = IswListViewSortDescending;

    /* IconView inside viewport */
    IswArgBuilderReset(&ab);
    IswArgBorderWidth(&ab, 0);
    IswArgIconSize(&ab, 64);
    IswArgItemSpacing(&ab, 16);
    IswArgMultiSelect(&ab, True);
    IswArgLabelLines(&ab, 3);
    fm->iconview = IswCreateManagedWidget("iconView", iconViewWidgetClass,
                                         fm->viewport, ab.args, ab.count);
    IswAddCallback(fm->iconview, IswNselectCallback, iconview_callback, fm);
    fm_register_context_menu(fm, fm->iconview);
    fm_install_shortcuts(fm->iconview);

    IswOverrideTranslations(fm->iconview, IswParseTranslationTable(
        "<Btn1Down>: SelectItem() fm-update-status()\n"
        "<Btn1Up>: BandFinish() fm-update-status()\n"
        "Ctrl<Key>a: SelectAll() fm-update-status()\n"
        "Shift<Key>Left: ExtendSelection(left) fm-update-status()\n"
        "Shift<Key>Right: ExtendSelection(right) fm-update-status()\n"
        "Shift<Key>Up: ExtendSelection(up) fm-update-status()\n"
        "Shift<Key>Down: ExtendSelection(down) fm-update-status()\n"
        "~Shift ~Ctrl<Key>Left: MoveCursor(left) fm-update-status()\n"
        "~Shift ~Ctrl<Key>Right: MoveCursor(right) fm-update-status()\n"
        "~Shift ~Ctrl<Key>Up: MoveCursor(up) fm-update-status()\n"
        "~Shift ~Ctrl<Key>Down: MoveCursor(down) fm-update-status()\n"
        "<Key>Home: MoveCursor(home) fm-update-status()\n"
        "<Key>End: MoveCursor(end) fm-update-status()\n"
        "<Key>space: ToggleCursor() fm-update-status()\n"));

    /* ListView inside viewport (initially unmanaged) */
    IswArgBuilderReset(&ab);
    IswArgBorderWidth(&ab, 0);
    IswArgListViewColumns(&ab, lv_columns);
    IswArgNumColumns(&ab, LV_NCOLS);
    IswArgMultiSelect(&ab, True);
    IswArgShowHeader(&ab, True);
    fm->listview = IswCreateWidget("listView", listViewWidgetClass,
                                  fm->viewport, ab.args, ab.count);
    IswAddCallback(fm->listview, IswNselectCallback, listview_callback, fm);
    IswAddCallback(fm->listview, IswNreorderCallback, reorder_callback, fm);
    fm_register_context_menu(fm, fm->listview);
    fm_install_shortcuts(fm->listview);

    IswOverrideTranslations(fm->listview, IswParseTranslationTable(
        "<Btn1Down>: SelectRow() fm-update-status()\n"
        "<Btn1Up>: BandFinish() fm-update-status()\n"
        "Ctrl<Key>a: SelectAll() fm-update-status()\n"
        "Shift<Key>Up: ExtendSelection(up) fm-update-status()\n"
        "Shift<Key>Down: ExtendSelection(down) fm-update-status()\n"
        "~Shift ~Ctrl<Key>Up: MoveCursor(up) fm-update-status()\n"
        "~Shift ~Ctrl<Key>Down: MoveCursor(down) fm-update-status()\n"
        "<Key>Home: MoveCursor(home) fm-update-status()\n"
        "<Key>End: MoveCursor(end) fm-update-status()\n"
        "<Key>space: ToggleCursor() fm-update-status()\n"));

    /* Default to icon view */
    fm->view_mode = FM_VIEW_ICON;

    /* Status bar */
    Widget statusbar = IswMainWindowGetStatusBar(fm->main_window);
    if (statusbar) {
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, "");
        IswArgBorderWidth(&ab, 0);
        IswArgStatusStretch(&ab, True);
        fm->status_label = IswCreateManagedWidget("status", labelWidgetClass,
                                                  statusbar, ab.args, ab.count);
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

static void format_iec_size(off_t size, char *buf, size_t len)
{
    if (size < 1024)
        snprintf(buf, len, "%ld B", (long)size);
    else if (size < 1024 * 1024)
        snprintf(buf, len, "%.1f KiB", size / 1024.0);
    else if (size < 1024L * 1024 * 1024)
        snprintf(buf, len, "%.1f MiB", size / (1024.0 * 1024));
    else if (size < 1024LL * 1024 * 1024 * 1024)
        snprintf(buf, len, "%.1f GiB", size / (1024.0 * 1024 * 1024));
    else
        snprintf(buf, len, "%.1f TiB", size / (1024.0 * 1024 * 1024 * 1024));
}

void fileview_update_status(Fm *fm)
{
    if (!fm->status_label)
        return;

    int ndirs = 0, nfiles = 0;
    off_t total_size = 0;
    for (int i = 0; i < fm->nentries; i++) {
        if (fm->entries[i].is_dir)
            ndirs++;
        else {
            nfiles++;
            total_size += fm->entries[i].size;
        }
    }

    char sizebuf[32];
    char buf[256];

    int *sel = NULL;
    int nsel = fileview_get_selected_items(fm, &sel);
    if (nsel > 0 && sel) {
        int sel_dirs = 0, sel_files = 0;
        off_t sel_size = 0;
        for (int i = 0; i < nsel; i++) {
            int idx = sel[i];
            if (idx < 0 || idx >= fm->nentries)
                continue;
            if (fm->entries[idx].is_dir)
                sel_dirs++;
            else {
                sel_files++;
                sel_size += fm->entries[idx].size;
            }
        }
        free(sel);

        format_iec_size(sel_size, sizebuf, sizeof(sizebuf));
        snprintf(buf, sizeof(buf),
                 "%d folders, %d files selected (%s)",
                 sel_dirs, sel_files, sizebuf);
    } else {
        free(sel);
        format_iec_size(total_size, sizebuf, sizeof(sizebuf));
        snprintf(buf, sizeof(buf), "%d folders, %d files (%s)",
                 ndirs, nfiles, sizebuf);
    }

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgLabel(&ab, buf);
    IswSetValues(fm->status_label, ab.args, ab.count);
}

void fileview_populate(Fm *fm)
{
    if (fm->view_mode == FM_VIEW_ICON && fm->iconview) {
        thumbs_apply_cache(fm);

        free(fm->fv_labels);
        free(fm->fv_icons);

        fm->fv_labels = malloc(fm->nentries * sizeof(String));
        fm->fv_icons  = malloc(fm->nentries * sizeof(String));

        for (int i = 0; i < fm->nentries; i++) {
            fm->fv_labels[i] = (String)fm->entries[i].name;
            fm->fv_icons[i]  = (String)icons_for_entry(fm->app_state,
                                                        &fm->entries[i]);
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

    fileview_update_status(fm);

    /* Kick off async thumbnail generation for image/video files */
    if (fm->view_mode == FM_VIEW_ICON)
        thumbs_populate_async(fm);
}

void fileview_cleanup(Fm *fm)
{
    free(fm->fv_labels);
    free(fm->fv_icons);
    fm->fv_labels = NULL;
    fm->fv_icons = NULL;

    lv_free_data(fm);
}
