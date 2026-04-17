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

/* ---------- Section tracking ---------- */

typedef struct {
    Widget  header;     /* Label widget for section title */
    Widget  list;       /* List widget for section items */
    int     start_idx;  /* index into places[] of first item */
    int     nitems;     /* number of items in this section */
    String *labels;     /* string array owned by us, pointed to by List */
} PlaceSection;

/* ---------- Per-window places data ---------- */

struct FmPlacesData {
    PlaceEntry   *places;
    int           nplaces;
    int           places_cap;

    PlaceSection *sections;
    int           nsections;
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

static void sections_free(FmPlacesData *pd)
{
    for (int i = 0; i < pd->nsections; i++) {
        free(pd->sections[i].labels);
    }
    free(pd->sections);
    pd->sections = NULL;
    pd->nsections = 0;
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

/* ---------- Build sections from flat place list ---------- */

static void build_sections(FmPlacesData *pd)
{
    sections_free(pd);

    /* Count sections (headers) */
    int count = 0;
    for (int i = 0; i < pd->nplaces; i++) {
        if (pd->places[i].is_header) { count++; }
    }

    pd->sections = calloc(count, sizeof(PlaceSection));
    pd->nsections = 0;

    for (int i = 0; i < pd->nplaces; i++) {
        if (!pd->places[i].is_header) {
            continue;
        }

        PlaceSection *s = &pd->sections[pd->nsections++];
        s->start_idx = i + 1;

        /* Count items until next header or end */
        s->nitems = 0;
        for (int j = i + 1; j < pd->nplaces && !pd->places[j].is_header; j++) {
            s->nitems++;
        }

        /* Build label array for the List widget */
        s->labels = calloc(s->nitems, sizeof(String));
        for (int j = 0; j < s->nitems; j++) {
            s->labels[j] = pd->places[s->start_idx + j].label;
        }
    }
}

/* ---------- Sidebar UI ---------- */

static void place_list_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w;
    Fm *fm = (Fm *)cd;
    FmPlacesData *pd = fm->places_data;
    IswListReturnStruct *ret = (IswListReturnStruct *)call;

    if (ret->list_index == XAW_LIST_NONE) {
        return;
    }

    /* Find which section this List widget belongs to */
    for (int i = 0; i < pd->nsections; i++) {
        if (pd->sections[i].list != w) {
            continue;
        }

        int idx = pd->sections[i].start_idx + ret->list_index;
        if (idx < pd->nplaces && pd->places[idx].path) {
            struct stat st;
            if (stat(pd->places[idx].path, &st) != 0)
                mkdir(pd->places[idx].path, 0755);
            fm_navigate(fm, pd->places[idx].path);
        }

        /* Unhighlight all other section lists */
        for (int j = 0; j < pd->nsections; j++) {
            if (j != i && pd->sections[j].list) {
                IswListUnhighlight(pd->sections[j].list);
            }
        }
        return;
    }
}

/* ---------- places sidebar drop ---------- */

static void places_vp_drop_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w;
    Fm *fm = (Fm *)cd;
    FmPlacesData *pd = fm->places_data;
    IswDropCallbackData *d = (IswDropCallbackData *)call;

    if (!pd)
        return;

    /* d->x/y from XDND are unreliable (broken coordinate translation).
     * Query the pointer directly against each list widget's window. */
    xcb_connection_t *conn = IswDisplay(fm->toplevel);
    const char *target_dir = NULL;

    for (int i = 0; i < pd->nsections; i++) {
        PlaceSection *s = &pd->sections[i];
        if (!s->list || s->nitems <= 0 || !IswIsRealized(s->list))
            continue;

        xcb_query_pointer_cookie_t qpc =
            xcb_query_pointer(conn, IswWindow(s->list));
        xcb_query_pointer_reply_t *qpr =
            xcb_query_pointer_reply(conn, qpc, NULL);
        if (!qpr)
            continue;

        int wx = qpr->win_x;
        int wy = qpr->win_y;
        free(qpr);

        /* Convert physical pixels to logical */
        double sf = ISWScaleFactor(fm->toplevel);
        wx = (int)(wx / sf + 0.5);
        wy = (int)(wy / sf + 0.5);

        if (wx < 0 || wy < 0 ||
            wx >= (int)s->list->core.width ||
            wy >= (int)s->list->core.height)
            continue;

        int row = wy * s->nitems / (int)s->list->core.height;
        if (row < 0) row = 0;
        if (row >= s->nitems) row = s->nitems - 1;

        int idx = s->start_idx + row;
        if (idx < pd->nplaces && pd->places[idx].path)
            target_dir = pd->places[idx].path;
        break;
    }

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

void places_init(Fm *fm)
{
    FmPlacesData *pd = calloc(1, sizeof(FmPlacesData));
    fm->places_data = pd;

    build_places_list(pd, fm->app_state);
    build_sections(pd);

    /* Create sidebar viewport */
    Arg args[20];
    Cardinal n = 0;
    IswSetArg(args[n], IswNallowVert, True);              n++;
    IswSetArg(args[n], IswNallowHoriz, False);             n++;
    IswSetArg(args[n], IswNuseRight, False);                n++;
    IswSetArg(args[n], IswNborderWidth, 1);                 n++;
    IswSetArg(args[n], IswNflexGrow, 0);                    n++;
    fm->places_vp = IswCreateManagedWidget("placesVp", viewportWidgetClass,
                                           fm->hbox, args, n);

    /* Vertical FlexBox inside viewport */
    n = 0;
    IswSetArg(args[n], IswNorientation, XtorientVertical); n++;
    IswSetArg(args[n], IswNborderWidth, 0);                 n++;
    IswSetArg(args[n], IswNspacing, 0);                     n++;
    fm->places_box = IswCreateManagedWidget("placesBox", flexBoxWidgetClass,
                                            fm->places_vp, args, n);

    /* Create header + list for each section */
    char wname[32];
    for (int i = 0; i < pd->nsections; i++) {
        PlaceSection *s = &pd->sections[i];
        int hdr_idx = s->start_idx - 1;

        /* Section header label */
        n = 0;
        snprintf(wname, sizeof(wname), "placeHdr%d", i);
        IswSetArg(args[n], IswNlabel, pd->places[hdr_idx].label); n++;
        IswSetArg(args[n], IswNborderWidth, 0);                n++;
        IswSetArg(args[n], IswNinternalWidth, 6);  n++;
        IswSetArg(args[n], IswNinternalHeight, 2); n++;
        IswSetArg(args[n], IswNjustify, IswJustifyLeft);        n++;
        s->header = IswCreateManagedWidget(wname, labelWidgetClass,
                                           fm->places_box, args, n);

        /* List widget for this section's items */
        if (s->nitems > 0) {
            n = 0;
            snprintf(wname, sizeof(wname), "placeList%d", i);
            IswSetArg(args[n], IswNlist, s->labels);                n++;
            IswSetArg(args[n], IswNnumberStrings, s->nitems);       n++;
            IswSetArg(args[n], IswNdefaultColumns, 1);              n++;
            IswSetArg(args[n], IswNforceColumns, True);             n++;
            IswSetArg(args[n], IswNverticalList, True);             n++;
            IswSetArg(args[n], IswNborderWidth, 0);                 n++;
            IswSetArg(args[n], IswNinternalWidth, 4);   n++;
            IswSetArg(args[n], IswNinternalHeight, 2);  n++;
            s->list = IswCreateManagedWidget(wname, listWidgetClass,
                                             fm->places_box, args, n);
            IswAddCallback(s->list, IswNcallback, place_list_cb, fm);
        }
    }

    /* Right-click handler on the Devices section list */
    PlaceSection *devs = (pd->nsections > 1) ? &pd->sections[1] : NULL;
    if (devs && devs->list) {
        IswAddRawEventHandler(devs->list, XCB_EVENT_MASK_BUTTON_PRESS, False,
                              dev_list_ctx_handler, fm);
    }
}

void places_register_drop_targets(Fm *fm)
{
    xcb_atom_t uri_type = ISWXdndInternType(fm->places_vp, "text/uri-list");
    ISWXdndWidgetAcceptDrops(fm->places_vp);
    ISWXdndSetDropCallback(fm->places_vp, places_vp_drop_cb, fm);
    ISWXdndSetAcceptedTypes(fm->places_vp, &uri_type, 1);
    ISWXdndSetAcceptedActions(fm->places_vp,
        ISW_DND_ACTION_COPY | ISW_DND_ACTION_MOVE);
}

/* Find the Devices section (index 1) */
static PlaceSection *devices_section(FmPlacesData *pd)
{
    return (pd->nsections > 1) ? &pd->sections[1] : NULL;
}

static void devices_update_list(PlaceSection *s, FmPlacesData *pd)
{
    free(s->labels);
    s->labels = calloc(s->nitems, sizeof(String));
    for (int j = 0; j < s->nitems; j++) {
        s->labels[j] = pd->places[s->start_idx + j].label;
    }
    fprintf(stderr, "isde-fm: devices_update_list nitems=%d list=%p\n",
            s->nitems, (void *)s->list);
    for (int k = 0; k < s->nitems; k++) {
        fprintf(stderr, "  [%d] %s\n", k, s->labels[k]);
    }
    if (s->list) {
        IswUnmanageChild(s->list);
        IswListChange(s->list, s->labels, s->nitems, 0, True);
        IswManageChild(s->list);
    }
}

void places_refresh_devices(Fm *fm)
{
    FmPlacesData *pd = fm->places_data;
    if (!pd)
        return;
    PlaceSection *s = devices_section(pd);
    if (!s)
        return;

    /* Remove all existing device entries (keep "File System" at index 0) */
    int keep = 1;  /* "File System" */
    for (int i = keep; i < s->nitems; i++) {
        int idx = s->start_idx + i;
        free(pd->places[idx].label);
        free(pd->places[idx].path);
        free(pd->places[idx].icon_name);
    }
    int removed = s->nitems - keep;
    if (removed > 0) {
        int after = s->start_idx + s->nitems;
        memmove(&pd->places[s->start_idx + keep],
                &pd->places[after],
                (pd->nplaces - after) * sizeof(PlaceEntry));
        pd->nplaces -= removed;
        s->nitems = keep;
        for (int i = 2; i < pd->nsections; i++)
            pd->sections[i].start_idx -= removed;
    }

    /* Re-add devices from mountd */
    FmApp *app = fm->app_state;
    for (int i = 0; i < app->mountd_ndevices; i++) {
        FmDeviceInfo *d = &app->mountd_devices[i];
        const char *name = device_display_name(d);
        const char *path = d->is_mounted ? d->mount_point : NULL;

        int ins = s->start_idx + s->nitems;
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

        s->nitems++;
        for (int j = 2; j < pd->nsections; j++)
            pd->sections[j].start_idx++;
    }

    devices_update_list(s, pd);

    /* Re-register right-click handler (list widget may have been recreated) */
    if (s->list) {
        IswAddRawEventHandler(s->list, XCB_EVENT_MASK_BUTTON_PRESS, False,
                              dev_list_ctx_handler, fm);
    }
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
    PlaceSection *s = devices_section(pd);
    if (!s)
        return;

    /* Check for duplicate */
    for (int i = 0; i < s->nitems; i++) {
        if (strcmp(pd->places[s->start_idx + i].label, name) == 0)
            return;
    }

    int ins = s->start_idx + s->nitems;
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

    s->nitems++;
    for (int i = 2; i < pd->nsections; i++)
        pd->sections[i].start_idx++;

    devices_update_list(s, pd);
}

void places_device_removed(Fm *fm, const char *name)
{
    FmPlacesData *pd = fm->places_data;
    if (!pd)
        return;
    PlaceSection *s = devices_section(pd);
    if (!s)
        return;

    int found = -1;
    for (int i = 0; i < s->nitems; i++) {
        if (strcmp(pd->places[s->start_idx + i].label, name) == 0) {
            found = s->start_idx + i;
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

    s->nitems--;
    for (int i = 2; i < pd->nsections; i++)
        pd->sections[i].start_idx--;

    devices_update_list(s, pd);
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

    Arg args[20];
    Cardinal n = 0;
    IswSetArg(args[n], IswNx, root_x);                 n++;
    IswSetArg(args[n], IswNy, root_y);                  n++;
    IswSetArg(args[n], IswNoverrideRedirect, True);     n++;
    IswSetArg(args[n], IswNborderWidth, 1);             n++;
    fm->dev_ctx_shell = IswCreatePopupShell("devCtxMenu",
                            overrideShellWidgetClass, fm->toplevel, args, n);

    n = 0;
    IswSetArg(args[n], IswNlist, dev_ctx_data.labels);  n++;
    IswSetArg(args[n], IswNnumberStrings, pos);         n++;
    IswSetArg(args[n], IswNdefaultColumns, 1);          n++;
    IswSetArg(args[n], IswNforceColumns, True);         n++;
    IswSetArg(args[n], IswNverticalList, True);         n++;
    IswSetArg(args[n], IswNborderWidth, 0);             n++;
    IswSetArg(args[n], IswNcursor, None);               n++;
    Widget list = IswCreateManagedWidget("devCtxList", listWidgetClass,
                                         fm->dev_ctx_shell, args, n);
    IswAddCallback(list, IswNcallback, dev_ctx_select_cb, NULL);

    static char translations[] =
        "<EnterWindow>: Set()\n"
        "<LeaveWindow>: Unset()\n"
        "<Motion>:      Set()\n"
        "<BtnDown>:     Set() Notify()\n"
        "<BtnUp>:       Notify()";
    IswOverrideTranslations(list, IswParseTranslationTable(translations));

    IswPopup(fm->dev_ctx_shell, IswGrabNone);
}

/* ---------- right-click handler for device list ---------- */

static void dev_list_ctx_handler(Widget w, IswPointer client_data,
                                 xcb_generic_event_t *event, Boolean *cont)
{
    (void)cont;
    if ((event->response_type & ~0x80) != XCB_BUTTON_PRESS)
        return;
    xcb_button_press_event_t *ev = (xcb_button_press_event_t *)event;
    if (ev->detail != 3)
        return;

    Fm *fm = (Fm *)client_data;
    FmPlacesData *pd = fm->places_data;
    if (!pd)
        return;

    PlaceSection *s = devices_section(pd);
    if (!s || s->list != w || s->nitems <= 0)
        return;

    int row = ev->event_y * s->nitems / (int)w->core.height;
    if (row < 0) row = 0;
    if (row >= s->nitems) row = s->nitems - 1;

    int idx = s->start_idx + row;
    dev_ctx_show(fm, idx, ev->root_x, ev->root_y);
}

void places_cleanup(Fm *fm)
{
    places_dismiss_device_menu(fm);
    FmPlacesData *pd = fm->places_data;
    if (!pd) {
        return;
    }
    sections_free(pd);
    places_free_entries(pd);
    free(pd);
    fm->places_data = NULL;
}
