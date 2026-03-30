#define _POSIX_C_SOURCE 200809L
#ifdef __FreeBSD__
#define __BSD_VISIBLE 1
#endif
/*
 * places.c — sidebar with XDG user dirs, filesystem locations, bookmarks
 */
#include "fm.h"

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
    for (int i = 0; i < pd->nsections; i++)
        free(pd->sections[i].labels);
    free(pd->sections);
    pd->sections = NULL;
    pd->nsections = 0;
}

/* ---------- Build place list ---------- */

static void add_xdg_dir(FmPlacesData *pd, const char *xdg_name,
                         const char *label, const char *icon_name)
{
    char *path = isde_xdg_user_dir(xdg_name);
    if (path) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            places_add(pd, label, path, icon_name, 0);
        }
        free(path);
    }
}

static void build_places_list(FmPlacesData *pd)
{
    places_free_entries(pd);

    /* --- Places section --- */
    places_add(pd, "Places", NULL, NULL, 1);

    const char *home = getenv("HOME");
    if (home)
        places_add(pd, "Home", home, "user-home", 0);

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

    /* Scan mounts for removable/user media */
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
            /* Show /media and /mnt mounts, skip virtual filesystems */
            if (strncmp(me->mnt_dir, "/media/", 7) == 0 ||
                (strncmp(me->mnt_dir, "/mnt/", 5) == 0 &&
                 strlen(me->mnt_dir) > 5)) {
                /* Use last path component as label */
                const char *name = strrchr(me->mnt_dir, '/');
                name = name ? name + 1 : me->mnt_dir;
                places_add(pd, name, me->mnt_dir, "drive-removable-media", 0);
            }
        }
        endmntent(fp_mnt);
    }
#endif

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
            if (nl) *nl = '\0';
            if (line[0] == '\0')
                continue;

            /* Format: file:///path [optional label] */
            char *uri = line;
            char *label = NULL;

            /* Check for space-separated label */
            char *space = strchr(line, ' ');
            if (space) {
                *space = '\0';
                label = space + 1;
                while (*label == ' ') label++;
                if (*label == '\0') label = NULL;
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
            if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
                continue;

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
    for (int i = 0; i < pd->nplaces; i++)
        if (pd->places[i].is_header) count++;

    pd->sections = calloc(count, sizeof(PlaceSection));
    pd->nsections = 0;

    for (int i = 0; i < pd->nplaces; i++) {
        if (!pd->places[i].is_header)
            continue;

        PlaceSection *s = &pd->sections[pd->nsections++];
        s->start_idx = i + 1;

        /* Count items until next header or end */
        s->nitems = 0;
        for (int j = i + 1; j < pd->nplaces && !pd->places[j].is_header; j++)
            s->nitems++;

        /* Build label array for the List widget */
        s->labels = calloc(s->nitems, sizeof(String));
        for (int j = 0; j < s->nitems; j++)
            s->labels[j] = pd->places[s->start_idx + j].label;
    }
}

/* ---------- Sidebar UI ---------- */

static void place_list_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w;
    Fm *fm = (Fm *)cd;
    FmPlacesData *pd = fm->places_data;
    IswListReturnStruct *ret = (IswListReturnStruct *)call;

    if (ret->list_index == XAW_LIST_NONE)
        return;

    /* Find which section this List widget belongs to */
    for (int i = 0; i < pd->nsections; i++) {
        if (pd->sections[i].list != w)
            continue;

        int idx = pd->sections[i].start_idx + ret->list_index;
        if (idx < pd->nplaces && pd->places[idx].path)
            fm_navigate(fm, pd->places[idx].path);

        /* Unhighlight all other section lists */
        for (int j = 0; j < pd->nsections; j++)
            if (j != i && pd->sections[j].list)
                IswListUnhighlight(pd->sections[j].list);
        return;
    }
}

void places_init(Fm *fm)
{
    FmPlacesData *pd = calloc(1, sizeof(FmPlacesData));
    fm->places_data = pd;

    build_places_list(pd);
    build_sections(pd);

    /* Create sidebar viewport */
    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNallowVert, True);              n++;
    XtSetArg(args[n], XtNallowHoriz, False);             n++;
    XtSetArg(args[n], XtNuseRight, False);                n++;
    XtSetArg(args[n], XtNborderWidth, 1);                 n++;
    XtSetArg(args[n], XtNflexGrow, 0);                    n++;
    fm->places_vp = XtCreateManagedWidget("placesVp", viewportWidgetClass,
                                           fm->hbox, args, n);

    /* Vertical FlexBox inside viewport */
    n = 0;
    XtSetArg(args[n], XtNorientation, XtorientVertical); n++;
    XtSetArg(args[n], XtNborderWidth, 0);                 n++;
    XtSetArg(args[n], XtNspacing, 0);                     n++;
    fm->places_box = XtCreateManagedWidget("placesBox", flexBoxWidgetClass,
                                            fm->places_vp, args, n);

    /* Create header + list for each section */
    char wname[32];
    for (int i = 0; i < pd->nsections; i++) {
        PlaceSection *s = &pd->sections[i];
        int hdr_idx = s->start_idx - 1;

        /* Section header label */
        n = 0;
        snprintf(wname, sizeof(wname), "placeHdr%d", i);
        XtSetArg(args[n], XtNlabel, pd->places[hdr_idx].label); n++;
        XtSetArg(args[n], XtNborderWidth, 0);                n++;
        XtSetArg(args[n], XtNinternalWidth, isde_scale(6));  n++;
        XtSetArg(args[n], XtNinternalHeight, isde_scale(2)); n++;
        XtSetArg(args[n], XtNjustify, XtJustifyLeft);        n++;
        s->header = XtCreateManagedWidget(wname, labelWidgetClass,
                                           fm->places_box, args, n);

        /* List widget for this section's items */
        if (s->nitems > 0) {
            n = 0;
            snprintf(wname, sizeof(wname), "placeList%d", i);
            XtSetArg(args[n], XtNlist, s->labels);                n++;
            XtSetArg(args[n], XtNnumberStrings, s->nitems);       n++;
            XtSetArg(args[n], XtNdefaultColumns, 1);              n++;
            XtSetArg(args[n], XtNforceColumns, True);             n++;
            XtSetArg(args[n], XtNverticalList, True);             n++;
            XtSetArg(args[n], XtNborderWidth, 0);                 n++;
            XtSetArg(args[n], XtNinternalWidth, isde_scale(4));   n++;
            XtSetArg(args[n], XtNinternalHeight, isde_scale(2));  n++;
            s->list = XtCreateManagedWidget(wname, listWidgetClass,
                                             fm->places_box, args, n);
            XtAddCallback(s->list, XtNcallback, place_list_cb, fm);
        }
    }
}

void places_cleanup(Fm *fm)
{
    FmPlacesData *pd = fm->places_data;
    if (!pd)
        return;
    sections_free(pd);
    places_free_entries(pd);
    free(pd);
    fm->places_data = NULL;
}
