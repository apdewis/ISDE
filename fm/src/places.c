#define _POSIX_C_SOURCE 200809L
#ifdef __FreeBSD__
#define __BSD_VISIBLE 1
#endif
/*
 * places.c — sidebar with XDG user dirs, filesystem locations, bookmarks
 */
#include "fm.h"
#include "fm_mountd.h"
#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>
#include <ISW/ListBox.h>
#include <ISW/ListBoxRow.h>


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __FreeBSD__
#include <sys/types.h>
#include <sys/param.h>
#include <sys/mount.h>
#else
#include <mntent.h>
#endif

/* ---------- Place entry ---------- */

typedef struct {
    char *label;
    char *path;
    char *icon_name;    /* freedesktop icon name */
    int   is_header;    /* section header, not clickable */
} PlaceEntry;

/* ---------- Per-window places data ---------- */

struct FmPlacesData {
    PlaceEntry *places;
    int         nplaces;
    int         places_cap;
    Widget     *row_widgets;    /* parallel to places[], one Label per entry */
    int         nrow_widgets;
    char      **icon_paths;     /* resolved SVG paths, parallel to places[] */
    int         nicon_paths;
};

static void places_add(FmPlacesData *pd, const char *label, const char *path,
                        const char *icon_name, int is_header)
{
    if (pd->nplaces >= pd->places_cap) {
        pd->places_cap = pd->places_cap ? pd->places_cap * 2 : 32;
        pd->places = realloc(pd->places, pd->places_cap * sizeof(PlaceEntry));
    }
    PlaceEntry *p = &pd->places[pd->nplaces++];
    p->label = strdup(label);
    p->path = path ? strdup(path) : NULL;
    p->icon_name = icon_name ? strdup(icon_name) : NULL;
    p->is_header = is_header;
}

static void places_free_entries(FmPlacesData *pd)
{
    for (int i = 0; i < pd->nplaces; i++) {
        free(pd->places[i].label);
        free(pd->places[i].path);
        free(pd->places[i].icon_name);
    }
    free(pd->places);
    pd->places = NULL;
    pd->nplaces = 0;
    pd->places_cap = 0;
}

/* ---------- Device display name: label > vendor > dev_path ---------- */

static const char *device_display_name(const FmDeviceInfo *d)
{
    if (d->label[0])  return d->label;
    if (d->vendor[0]) return d->vendor;
    return d->dev_path;
}

/* ---------- Build place list ---------- */

static void add_xdg_dir(FmPlacesData *pd, const char *xdg_name,
                         const char *label, const char *icon_name)
{
    char *path = isde_xdg_user_dir(xdg_name);
    if (path) {
        places_add(pd, label, path, icon_name, 0);
        free(path);
    }
}

static void build_places_list(FmPlacesData *pd, FmApp *app)
{
    places_free_entries(pd);

    /* --- Places section --- */
    places_add(pd, "Places", NULL, NULL, 1);

    const char *home = getenv("HOME");
    if (home) {
        places_add(pd, "Home", home, "user-home", 0);
    }

    add_xdg_dir(pd, "DESKTOP",   "Desktop",   "user-desktop");
    add_xdg_dir(pd, "DOCUMENTS", "Documents", "folder-documents");
    add_xdg_dir(pd, "DOWNLOAD",  "Downloads", "folder-download");
    add_xdg_dir(pd, "MUSIC",     "Music",     "folder-music");
    add_xdg_dir(pd, "PICTURES",  "Pictures",  "folder-pictures");
    add_xdg_dir(pd, "VIDEOS",    "Videos",    "folder-videos");

    /* Trash */
    char *trash_path = fileops_trash_path();
    places_add(pd, "Trash", trash_path, "user-trash", 0);
    free(trash_path);

    /* --- Devices section --- */
    places_add(pd, "Devices", NULL, NULL, 1);
    places_add(pd, "File System", "/", "drive-harddisk", 0);

    /* Populate removable devices from mountd if available,
     * otherwise fall back to scanning /proc/mounts. */
    if (app->has_mountd) {
        for (int i = 0; i < app->mountd_ndevices; i++) {
            FmDeviceInfo *d = &app->mountd_devices[i];
            const char *name = device_display_name(d);
            const char *path = d->is_mounted ? d->mount_point : NULL;
            places_add(pd, name, path, "drive-removable-media", 0);
        }
    } else {
#ifdef __FreeBSD__
        {
            struct statfs *mounts;
            int n = getmntinfo(&mounts, MNT_NOWAIT);
            for (int i = 0; i < n; i++) {
                const char *dir = mounts[i].f_mntonname;
                if (strncmp(dir, "/media/", 7) == 0 ||
                    (strncmp(dir, "/mnt/", 5) == 0 &&
                     strlen(dir) > 5)) {
                    const char *name = strrchr(dir, '/');
                    name = name ? name + 1 : dir;
                    places_add(pd, name, dir, "drive-removable-media", 0);
                }
            }
        }
#else
        FILE *fp_mnt = setmntent("/proc/mounts", "r");
        if (fp_mnt) {
            struct mntent *me;
            while ((me = getmntent(fp_mnt))) {
                if (strncmp(me->mnt_dir, "/media/", 7) == 0 ||
                    (strncmp(me->mnt_dir, "/mnt/", 5) == 0 &&
                     strlen(me->mnt_dir) > 5)) {
                    const char *name = strrchr(me->mnt_dir, '/');
                    name = name ? name + 1 : me->mnt_dir;
                    places_add(pd, name, me->mnt_dir,
                               "drive-removable-media", 0);
                }
            }
            endmntent(fp_mnt);
        }
#endif
    }

    /* --- Bookmarks section --- */
    FILE *fp;
    char bm_path[512];
    snprintf(bm_path, sizeof(bm_path), "%s/isde/bookmarks",
             isde_xdg_config_home());
    fp = fopen(bm_path, "r");
    if (!fp) {
        /* Try GTK bookmarks as fallback */
        snprintf(bm_path, sizeof(bm_path), "%s/gtk-3.0/bookmarks",
                 isde_xdg_config_home());
        fp = fopen(bm_path, "r");
    }
    if (fp) {
        int header_added = 0;
        char line[1024];
        while (fgets(line, sizeof(line), fp)) {
            /* Strip newline */
            char *nl = strchr(line, '\n');
            if (nl) { *nl = '\0'; }
            if (line[0] == '\0') {
                continue;
            }

            /* Format: file:///path [optional label] */
            char *uri = line;
            char *label = NULL;

            /* Check for space-separated label */
            char *space = strchr(line, ' ');
            if (space) {
                *space = '\0';
                label = space + 1;
                while (*label == ' ') { label++; }
                if (*label == '\0') { label = NULL; }
            }

            /* Convert file:// URI to path */
            char *path = NULL;
            if (strncmp(uri, "file://", 7) == 0) {
                path = uri + 7;
            } else if (uri[0] == '/') {
                path = uri;
            } else {
                continue;
            }

            struct stat st;
            if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
                continue;
            }

            if (!header_added) {
                places_add(pd, "Bookmarks", NULL, NULL, 1);
                header_added = 1;
            }

            if (!label) {
                label = strrchr(path, '/');
                label = label ? label + 1 : path;
            }
            places_add(pd, label, path, "folder-bookmark", 0);
        }
        fclose(fp);
    }
}

/* ---------- Sidebar UI ---------- */

static void place_list_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w;
    Fm *fm = (Fm *)cd;
    FmPlacesData *pd = fm->places_data;
    IswListBoxCallbackData *cb = (IswListBoxCallbackData *)call;

    int idx = cb->index;
    if (idx < 0 || idx >= pd->nplaces)
        return;
    if (pd->places[idx].is_header || !pd->places[idx].path)
        return;

    struct stat st;
    if (stat(pd->places[idx].path, &st) != 0)
        mkdir(pd->places[idx].path, 0755);
    fm_navigate(fm, pd->places[idx].path);
}

/* ---------- places sidebar hit-test ---------- */

/* Hit-test the places sidebar: query the pointer against each row widget
 * in the ListBox. Returns the target directory path, or NULL if no valid
 * place is under the cursor. If idx_out is non-NULL, receives the places
 * array index for highlight purposes. */
static const char *
places_hit_test(Fm *fm, int *idx_out)
{
    FmPlacesData *pd = fm->places_data;
    if (!pd || !fm->places_listbox || !IswIsRealized(fm->places_listbox))
        return NULL;

    xcb_connection_t *conn = IswDisplay(fm->toplevel);
    xcb_query_pointer_cookie_t qpc =
        xcb_query_pointer(conn, IswWindow(fm->places_listbox));
    xcb_query_pointer_reply_t *qpr =
        xcb_query_pointer_reply(conn, qpc, NULL);
    if (!qpr)
        return NULL;

    int wy = qpr->win_y;
    free(qpr);

    double sf = ISWScaleFactor(fm->toplevel);
    wy = (int)(wy / sf + 0.5);

    for (int i = 0; i < pd->nplaces; i++) {
        Widget rw = pd->row_widgets[i];
        if (!rw || !IswIsManaged(rw))
            continue;
        int cy = rw->core.y;
        int ch = (int)rw->core.height + 2 * (int)rw->core.border_width;
        if (wy >= cy && wy < cy + ch) {
            if (pd->places[i].is_header || !pd->places[i].path)
                return NULL;
            if (idx_out) *idx_out = i;
            return pd->places[i].path;
        }
    }

    return NULL;
}

static void places_clear_drop_highlight(Fm *fm)
{
    if (fm->places_listbox)
        IswListBoxClearSelection(fm->places_listbox);
}

/* ---------- places sidebar drag-over callbacks ---------- */

static void places_drag_motion_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w;
    Fm *fm = (Fm *)cd;
    FmPlacesData *pd = fm->places_data;
    (void)call;

    int idx = -1;
    const char *target = places_hit_test(fm, &idx);

    places_clear_drop_highlight(fm);
    if (target && idx >= 0 && idx < pd->nplaces && pd->row_widgets[idx])
        IswListBoxSelectChild(fm->places_listbox, pd->row_widgets[idx]);
}

static void places_drag_leave_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)call;
    places_clear_drop_highlight((Fm *)cd);
}

/* ---------- places sidebar drop ---------- */

static void places_vp_drop_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w;
    Fm *fm = (Fm *)cd;
    IswDropCallbackData *d = (IswDropCallbackData *)call;

    places_clear_drop_highlight(fm);

    const char *target_dir = places_hit_test(fm, NULL);
    if (!target_dir)
        return;

    /* Create the directory if needed (XDG user dirs) */
    struct stat st;
    if (stat(target_dir, &st) != 0)
        mkdir(target_dir, 0755);

    /* Extract URIs — fallback for intra-process drops where the
     * selection type doesn't match text/uri-list */
    char **local_uris = NULL;
    int num_uris = d->num_uris;
    char **uris = d->uris;

    if (num_uris <= 0 && d->data && d->data_length > 0) {
        const char *p = (const char *)d->data;
        const char *end = p + d->data_length;
        int cap = 0;
        while (p < end) {
            const char *nl = memchr(p, '\n', end - p);
            if (!nl) nl = end;
            size_t len = nl - p;
            if (len > 0 && p[len - 1] == '\r') len--;
            if (len > 0 && p[0] != '#') {
                if (num_uris >= cap) {
                    cap = cap ? cap * 2 : 8;
                    local_uris = realloc(local_uris, cap * sizeof(char *));
                }
                local_uris[num_uris++] = strndup(p, len);
            }
            p = nl + 1;
        }
        uris = local_uris;
    }

    if (num_uris <= 0)
        return;

    char **paths = malloc(num_uris * sizeof(char *));
    int npaths = 0;
    for (int i = 0; i < num_uris; i++) {
        const char *path = uris[i];
        if (strncmp(path, "file://", 7) == 0)
            path += 7;
        if (path[0] == '/')
            paths[npaths++] = (char *)path;
    }

    if (npaths > 0)
        jobqueue_submit_copy(fm->app_state, fm, paths, npaths, target_dir);

    free(paths);
    if (local_uris) {
        for (int i = 0; i < num_uris; i++)
            free(local_uris[i]);
        free(local_uris);
    }
}

/* Forward declaration */
static void dev_list_ctx_handler(Widget w, IswPointer client_data,
                                 xcb_generic_event_t *event, Boolean *cont);

/* Determine if places[idx] is the last non-header item before the next
 * header or end-of-list — used to set the separator constraint. */
static int is_last_in_section(FmPlacesData *pd, int idx)
{
    if (pd->places[idx].is_header)
        return 0;
    int next = idx + 1;
    return (next >= pd->nplaces || pd->places[next].is_header);
}

static const char *icon_category(const char *icon_name)
{
    if (!icon_name) return NULL;
    if (strncmp(icon_name, "drive-", 6) == 0) return "devices";
    return "places";
}

static char *resolve_place_icon(const char *theme, const char *icon_name)
{
    if (!icon_name) return NULL;
    const char *cat = icon_category(icon_name);
    if (!cat) return NULL;

    if (theme) {
        char *path = isde_icon_theme_lookup(theme, cat, icon_name);
        if (path) return path;
    }
    return isde_icon_find(cat, icon_name);
}

static void free_icon_paths(FmPlacesData *pd)
{
    for (int i = 0; i < pd->nicon_paths; i++)
        free(pd->icon_paths[i]);
    free(pd->icon_paths);
    pd->icon_paths = NULL;
    pd->nicon_paths = 0;
}

/* Destroy all row widgets and rebuild from the places[] array. */
static void rebuild_listbox_children(Fm *fm)
{
    FmPlacesData *pd = fm->places_data;
    Widget listbox = fm->places_listbox;

    /* Destroy existing children */
    if (pd->row_widgets) {
        for (int i = 0; i < pd->nrow_widgets; i++) {
            if (pd->row_widgets[i])
                IswDestroyWidget(pd->row_widgets[i]);
        }
    }

    free_icon_paths(pd);

    pd->row_widgets = realloc(pd->row_widgets, pd->nplaces * sizeof(Widget));
    pd->nrow_widgets = pd->nplaces;
    pd->icon_paths = calloc(pd->nplaces, sizeof(char *));
    pd->nicon_paths = pd->nplaces;

    const char *theme = fm->app_state->icon_theme;

    IswArgBuilder ab = IswArgBuilderInit();
    char wname[32];
    for (int i = 0; i < pd->nplaces; i++) {
        PlaceEntry *p = &pd->places[i];

        if (p->is_header) {
            IswArgBuilderReset(&ab);
            snprintf(wname, sizeof(wname), "placeRow%d", i);
            IswArgLabel(&ab, p->label);
            IswArgBorderWidth(&ab, 0);
            IswArgBuilderAdd(&ab, IswNlistBoxRowHeight, (IswArgVal)24);
            IswArgJustify(&ab, IswJustifyLeft);
            IswArgSelectable(&ab, False);
            pd->row_widgets[i] = IswCreateManagedWidget(wname, labelWidgetClass,
                                                         listbox, ab.args, ab.count);
        } else {
            IswArgBuilderReset(&ab);
            snprintf(wname, sizeof(wname), "placeRow%d", i);
            IswArgLabel(&ab, "");
            IswArgBuilderAdd(&ab, IswNlistBoxRowHeight, (IswArgVal)24);
            IswArgJustify(&ab, IswJustifyLeft);
            IswArgBorderWidth(&ab, 0);
            IswArgRowPadding(&ab, 0);
            pd->row_widgets[i] = IswCreateWidget(wname, listBoxRowWidgetClass,
                                                  listbox, ab.args, ab.count);

            pd->icon_paths[i] = resolve_place_icon(theme, p->icon_name);
            if (pd->icon_paths[i]) {
                IswArgBuilderReset(&ab);
                IswArgImage(&ab, pd->icon_paths[i]);
                IswArgBorderWidth(&ab, 0);
                IswArgInternalWidth(&ab, 2);
                IswCreateManagedWidget("icon", labelWidgetClass,
                                       pd->row_widgets[i], ab.args, ab.count);
            }

            IswArgBuilderReset(&ab);
            IswArgLabel(&ab, p->label);
            IswArgBorderWidth(&ab, 0);
            IswArgInternalWidth(&ab, 2);
            IswCreateManagedWidget("label", labelWidgetClass,
                                   pd->row_widgets[i], ab.args, ab.count);

            IswManageChild(pd->row_widgets[i]);
        }
    }
}

void places_init(Fm *fm)
{
    FmPlacesData *pd = calloc(1, sizeof(FmPlacesData));
    fm->places_data = pd;

    build_places_list(pd, fm->app_state);

    /* Create sidebar viewport */
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgAllowVert(&ab, True);
    IswArgAllowHoriz(&ab, False);
    IswArgUseRight(&ab, False);
    IswArgBorderWidth(&ab, 1);
    IswArgFlexGrow(&ab, 0);
    IswArgWidth(&ab, 180);
    fm->places_vp = IswCreateManagedWidget("placesVp", viewportWidgetClass,
                                           fm->hbox, ab.args, ab.count);

    /* Single ListBox inside viewport */
    IswArgBuilderReset(&ab);
    IswArgSelectionMode(&ab, IswListBoxSelectSingle);
    IswArgShowSeparators(&ab, True);
    IswArgRowSpacing(&ab, 0);
    IswArgBorderWidth(&ab, 0);
    fm->places_listbox = IswCreateManagedWidget("placesListBox",
                             listBoxWidgetClass, fm->places_vp,
                             ab.args, ab.count);

    IswAddCallback(fm->places_listbox, IswNselectCallback, place_list_cb, fm);
    IswAddRawEventHandler(fm->places_listbox, XCB_EVENT_MASK_BUTTON_PRESS,
                          False, dev_list_ctx_handler, fm);

    rebuild_listbox_children(fm);
}

void places_register_drop_targets(Fm *fm)
{
    xcb_atom_t uri_type = ISWXdndInternType(fm->places_vp, "text/uri-list");
    ISWXdndWidgetAcceptDrops(fm->places_vp);
    ISWXdndSetDropCallback(fm->places_vp, places_vp_drop_cb, fm);
    ISWXdndSetDragMotionCallback(fm->places_vp, places_drag_motion_cb, fm);
    ISWXdndSetDragLeaveCallback(fm->places_vp, places_drag_leave_cb, fm);
    ISWXdndSetAcceptedTypes(fm->places_vp, &uri_type, 1);
    ISWXdndSetAcceptedActions(fm->places_vp,
        ISW_DND_ACTION_COPY | ISW_DND_ACTION_MOVE);
}

/* Find the index of the "Devices" header in the flat array */
static int devices_header_idx(FmPlacesData *pd)
{
    int hdr = 0;
    for (int i = 0; i < pd->nplaces; i++) {
        if (pd->places[i].is_header) {
            hdr++;
            if (hdr == 2) return i;
        }
    }
    return -1;
}

/* Return the range [start, end) of device items (excludes header) */
static void devices_range(FmPlacesData *pd, int *start, int *end)
{
    int hdr = devices_header_idx(pd);
    if (hdr < 0) { *start = *end = 0; return; }
    *start = hdr + 1;
    *end = *start;
    while (*end < pd->nplaces && !pd->places[*end].is_header)
        (*end)++;
}

void places_refresh_devices(Fm *fm)
{
    FmPlacesData *pd = fm->places_data;
    if (!pd)
        return;

    int ds, de;
    devices_range(pd, &ds, &de);
    if (ds >= de && ds == 0)
        return;

    /* Remove all existing device entries except "File System" (index 0) */
    int keep = 1;
    for (int i = ds + keep; i < de; i++) {
        free(pd->places[i].label);
        free(pd->places[i].path);
        free(pd->places[i].icon_name);
    }
    int removed = (de - ds) - keep;
    if (removed > 0) {
        memmove(&pd->places[ds + keep], &pd->places[de],
                (pd->nplaces - de) * sizeof(PlaceEntry));
        pd->nplaces -= removed;
    }

    /* Re-add devices from mountd */
    FmApp *app = fm->app_state;
    int ins = ds + keep;
    for (int i = 0; i < app->mountd_ndevices; i++) {
        FmDeviceInfo *d = &app->mountd_devices[i];
        const char *name = device_display_name(d);
        const char *path = d->is_mounted ? d->mount_point : NULL;

        if (pd->nplaces >= pd->places_cap) {
            pd->places_cap = pd->places_cap ? pd->places_cap * 2 : 32;
            pd->places = realloc(pd->places,
                                 pd->places_cap * sizeof(PlaceEntry));
        }
        memmove(&pd->places[ins + 1], &pd->places[ins],
                (pd->nplaces - ins) * sizeof(PlaceEntry));
        pd->nplaces++;

        PlaceEntry *p = &pd->places[ins];
        p->label = strdup(name);
        p->path = path ? strdup(path) : NULL;
        p->icon_name = strdup("drive-removable-media");
        p->is_header = 0;
        ins++;
    }

    rebuild_listbox_children(fm);
}

/* ---------- device context menu ---------- */

/* Client data for device context menu */
#define DEV_CTX_MAX_ITEMS 8
typedef struct {
    Fm     *fm;
    char    dev_path[FM_DEV_PATH_LEN];
    char    mount_point[FM_MOUNT_POINT_LEN];
    String  labels[DEV_CTX_MAX_ITEMS];
    int     actions[DEV_CTX_MAX_ITEMS];
    int     nlabels;
} DevCtxData;

static DevCtxData dev_ctx_data;

void places_dismiss_device_menu(Fm *fm)
{
    if (fm->dev_ctx_shell) {
        IswPopdown(fm->dev_ctx_shell);
        IswDestroyWidget(fm->dev_ctx_shell);
        fm->dev_ctx_shell = NULL;
    }
}

/* ---------- menu selection callback ---------- */

enum {
    DEV_ACT_OPEN_NEW_WINDOW,
    DEV_ACT_MOUNT,
    DEV_ACT_UNMOUNT,
    DEV_ACT_EJECT,
};

static void dev_ctx_select_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w; (void)cd;
    IswListReturnStruct *ret = (IswListReturnStruct *)call;
    if (ret->list_index == XAW_LIST_NONE)
        return;

    int action = dev_ctx_data.actions[ret->list_index];
    Fm *fm = dev_ctx_data.fm;
    char result[256];

    /* Popdown only — don't destroy the shell while inside its callback.
     * fm_dismiss_context will destroy it on next interaction. */
    if (fm->dev_ctx_shell)
        IswPopdown(fm->dev_ctx_shell);

    switch (action) {
    case DEV_ACT_OPEN_NEW_WINDOW:
        if (dev_ctx_data.mount_point[0])
            fm_window_new(fm->app_state, dev_ctx_data.mount_point);
        break;
    case DEV_ACT_MOUNT: {
        FmApp *app = fm->app_state;
        if (fm_mountd_mount(app, dev_ctx_data.dev_path,
                            result, sizeof(result)) == 0) {
            fprintf(stderr, "isde-fm: mounted %s at %s\n",
                    dev_ctx_data.dev_path, result);
            /* Update device array immediately */
            FmDeviceInfo *d = fm_mountd_find_by_label(app,
                                  dev_ctx_data.dev_path);
            if (!d) {
                for (int i = 0; i < app->mountd_ndevices; i++) {
                    if (strcmp(app->mountd_devices[i].dev_path,
                              dev_ctx_data.dev_path) == 0) {
                        d = &app->mountd_devices[i];
                        break;
                    }
                }
            }
            if (d) {
                d->is_mounted = 1;
                snprintf(d->mount_point, sizeof(d->mount_point),
                         "%s", result);
            }
            for (int i = 0; i < app->nwindows; i++)
                places_refresh_devices(app->windows[i]);
        } else {
            fprintf(stderr, "isde-fm: mount failed: %s\n", result);
        }
        break;
    }
    case DEV_ACT_UNMOUNT: {
        FmApp *app = fm->app_state;
        if (fm_mountd_unmount(app, dev_ctx_data.dev_path,
                              result, sizeof(result)) == 0) {
            fprintf(stderr, "isde-fm: unmounted %s\n", dev_ctx_data.dev_path);
            for (int i = 0; i < app->mountd_ndevices; i++) {
                if (strcmp(app->mountd_devices[i].dev_path,
                           dev_ctx_data.dev_path) == 0) {
                    app->mountd_devices[i].is_mounted = 0;
                    app->mountd_devices[i].mount_point[0] = '\0';
                    break;
                }
            }
            for (int i = 0; i < app->nwindows; i++)
                places_refresh_devices(app->windows[i]);
        } else {
            fprintf(stderr, "isde-fm: unmount failed: %s\n", result);
        }
        break;
    }
    case DEV_ACT_EJECT: {
        FmApp *app = fm->app_state;
        if (fm_mountd_eject(app, dev_ctx_data.dev_path,
                            result, sizeof(result)) == 0) {
            fprintf(stderr, "isde-fm: ejected %s\n", dev_ctx_data.dev_path);
            for (int i = 0; i < app->mountd_ndevices; i++) {
                if (strcmp(app->mountd_devices[i].dev_path,
                           dev_ctx_data.dev_path) == 0) {
                    app->mountd_devices[i].is_mounted = 0;
                    app->mountd_devices[i].mount_point[0] = '\0';
                    break;
                }
            }
            for (int i = 0; i < app->nwindows; i++)
                places_refresh_devices(app->windows[i]);
        } else {
            fprintf(stderr, "isde-fm: eject failed: %s\n", result);
        }
        break;
    }
    }
}

/* ---------- inotify fallback: add/remove individual devices ---------- */

void places_device_added(Fm *fm, const char *name, const char *path)
{
    FmPlacesData *pd = fm->places_data;
    if (!pd)
        return;

    int ds, de;
    devices_range(pd, &ds, &de);

    /* Check for duplicate */
    for (int i = ds; i < de; i++) {
        if (strcmp(pd->places[i].label, name) == 0)
            return;
    }

    int ins = de;
    if (pd->nplaces >= pd->places_cap) {
        pd->places_cap = pd->places_cap ? pd->places_cap * 2 : 32;
        pd->places = realloc(pd->places, pd->places_cap * sizeof(PlaceEntry));
    }
    memmove(&pd->places[ins + 1], &pd->places[ins],
            (pd->nplaces - ins) * sizeof(PlaceEntry));
    pd->nplaces++;

    PlaceEntry *p = &pd->places[ins];
    p->label = strdup(name);
    p->path = path ? strdup(path) : NULL;
    p->icon_name = strdup("drive-removable-media");
    p->is_header = 0;

    rebuild_listbox_children(fm);
}

void places_device_removed(Fm *fm, const char *name)
{
    FmPlacesData *pd = fm->places_data;
    if (!pd)
        return;

    int ds, de;
    devices_range(pd, &ds, &de);

    int found = -1;
    for (int i = ds; i < de; i++) {
        if (strcmp(pd->places[i].label, name) == 0) {
            found = i;
            break;
        }
    }
    if (found < 0)
        return;

    free(pd->places[found].label);
    free(pd->places[found].path);
    free(pd->places[found].icon_name);
    memmove(&pd->places[found], &pd->places[found + 1],
            (pd->nplaces - found - 1) * sizeof(PlaceEntry));
    pd->nplaces--;

    rebuild_listbox_children(fm);
}

/* ---------- build and show device context menu ---------- */

static void dev_ctx_show(Fm *fm, int places_idx,
                         Position root_x, Position root_y)
{
    places_dismiss_device_menu(fm);

    FmPlacesData *pd = fm->places_data;
    if (!pd || places_idx < 0 || places_idx >= pd->nplaces)
        return;

    const char *label = pd->places[places_idx].label;
    const char *path = pd->places[places_idx].path;
    if (strcmp(label, "File System") == 0)
        return;

    FmApp *app = fm->app_state;
    FmDeviceInfo *dev = NULL;
    if (app->has_mountd) {
        if (path)
            dev = fm_mountd_find_by_mount_point(app, path);
        if (!dev)
            dev = fm_mountd_find_by_label(app, label);
    }

    /* Store context */
    dev_ctx_data.fm = fm;
    dev_ctx_data.nlabels = 0;
    if (dev) {
        snprintf(dev_ctx_data.dev_path, sizeof(dev_ctx_data.dev_path),
                 "%s", dev->dev_path);
        snprintf(dev_ctx_data.mount_point, sizeof(dev_ctx_data.mount_point),
                 "%s", dev->mount_point);
    } else {
        dev_ctx_data.dev_path[0] = '\0';
        snprintf(dev_ctx_data.mount_point, sizeof(dev_ctx_data.mount_point),
                 "%s", path ? path : "");
    }

    int is_mounted = dev ? dev->is_mounted : (path && path[0]);
    int pos = 0;

    if (is_mounted) {
        dev_ctx_data.labels[pos] = "Open in New Window";
        dev_ctx_data.actions[pos] = DEV_ACT_OPEN_NEW_WINDOW;
        pos++;
    }

    if (dev) {
        if (is_mounted) {
            dev_ctx_data.labels[pos] = "---";
            dev_ctx_data.actions[pos] = -1;
            pos++;
            dev_ctx_data.labels[pos] = "Unmount";
            dev_ctx_data.actions[pos] = DEV_ACT_UNMOUNT;
            pos++;
        } else {
            dev_ctx_data.labels[pos] = "Mount";
            dev_ctx_data.actions[pos] = DEV_ACT_MOUNT;
            pos++;
        }
        if (dev->is_ejectable) {
            dev_ctx_data.labels[pos] = "Eject";
            dev_ctx_data.actions[pos] = DEV_ACT_EJECT;
            pos++;
        }
    }

    if (pos == 0)
        return;

    dev_ctx_data.nlabels = pos;

    double sf = ISWScaleFactor(fm->toplevel);
    Position px = root_x;
    Position py = root_y;

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgOverrideRedirect(&ab, True);
    IswArgBorderWidth(&ab, 1);
    fm->dev_ctx_shell = IswCreatePopupShell("devCtxMenu",
                            overrideShellWidgetClass, fm->toplevel, ab.args, ab.count);

    IswArgBuilderReset(&ab);
    IswArgList(&ab, dev_ctx_data.labels);
    IswArgNumberStrings(&ab, pos);
    IswArgDefaultColumns(&ab, 1);
    IswArgForceColumns(&ab, True);
    IswArgVerticalList(&ab, True);
    IswArgBorderWidth(&ab, 0);
    IswArgCursor(&ab, None);
    Widget list = IswCreateManagedWidget("devCtxList", listWidgetClass,
                                         fm->dev_ctx_shell, ab.args, ab.count);
    IswAddCallback(list, IswNcallback, dev_ctx_select_cb, NULL);

    static char translations[] =
        "<EnterWindow>: Set()\n"
        "<LeaveWindow>: Unset()\n"
        "<Motion>:      Set()\n"
        "<BtnDown>:     Set() Notify()\n"
        "<BtnUp>:       Notify()";
    IswOverrideTranslations(list, IswParseTranslationTable(translations));

    IswRealizeWidget(fm->dev_ctx_shell);

    xcb_screen_t *scr = IswScreen(fm->toplevel);
    int scr_w = (int)(scr->width_in_pixels / sf);
    int scr_h = (int)(scr->height_in_pixels / sf);
    Dimension mw = fm->dev_ctx_shell->core.width;
    Dimension mh = fm->dev_ctx_shell->core.height;
    Dimension bw = fm->dev_ctx_shell->core.border_width;
    int menu_w = (int)mw + 2 * (int)bw;
    int menu_h = (int)mh + 2 * (int)bw;

    Position rx = px + 1;
    Position ry = py;
    if ((int)rx + menu_w > scr_w)
        rx = (Position)((int)px - menu_w);
    if ((int)ry + menu_h > scr_h)
        ry = (Position)(scr_h - menu_h);
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;

    IswMoveWidget(fm->dev_ctx_shell, rx, ry);
    IswPopup(fm->dev_ctx_shell, IswGrabNone);
}

/* ---------- right-click handler for device list ---------- */

static void dev_list_ctx_handler(Widget w, IswPointer client_data,
                                 xcb_generic_event_t *event, Boolean *cont)
{
    (void)w; (void)cont;
    if ((event->response_type & ~0x80) != XCB_BUTTON_PRESS)
        return;
    xcb_button_press_event_t *ev = (xcb_button_press_event_t *)event;
    if (ev->detail != 3)
        return;

    Fm *fm = (Fm *)client_data;
    FmPlacesData *pd = fm->places_data;
    if (!pd)
        return;

    /* Hit-test click position against row widgets */
    int wy = ev->event_y;
    double sf = ISWScaleFactor(fm->toplevel);
    wy = (int)(wy / sf + 0.5);

    int hit = -1;
    for (int i = 0; i < pd->nplaces; i++) {
        Widget rw = pd->row_widgets[i];
        if (!rw || !IswIsManaged(rw))
            continue;
        int cy = rw->core.y;
        int ch = (int)rw->core.height + 2 * (int)rw->core.border_width;
        if (wy >= cy && wy < cy + ch) {
            hit = i;
            break;
        }
    }
    if (hit < 0)
        return;

    /* Only show context menu for device items */
    int ds, de;
    devices_range(pd, &ds, &de);
    if (hit < ds || hit >= de)
        return;

    dev_ctx_show(fm, hit, ev->root_x, ev->root_y);
}

void places_cleanup(Fm *fm)
{
    places_dismiss_device_menu(fm);
    FmPlacesData *pd = fm->places_data;
    if (!pd) {
        return;
    }
    free(pd->row_widgets);
    free_icon_paths(pd);
    places_free_entries(pd);
    free(pd);
    fm->places_data = NULL;
}
