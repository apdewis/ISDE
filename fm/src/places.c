#define _POSIX_C_SOURCE 200809L
/*
 * places.c — sidebar with XDG user dirs, filesystem locations, bookmarks
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <mntent.h>

/* ---------- Place entry ---------- */

typedef struct {
    char *label;
    char *path;
    char *icon_name;    /* freedesktop icon name */
    int   is_header;    /* section header, not clickable */
} PlaceEntry;

static PlaceEntry *places = NULL;
static int          nplaces = 0;
static int          places_cap = 0;

/* ---------- Section tracking ---------- */

typedef struct {
    Widget  header;     /* Label widget for section title */
    Widget  list;       /* List widget for section items */
    int     start_idx;  /* index into places[] of first item */
    int     nitems;     /* number of items in this section */
    String *labels;     /* string array owned by us, pointed to by List */
} PlaceSection;

static PlaceSection *sections = NULL;
static int            nsections = 0;

static void places_add(const char *label, const char *path,
                        const char *icon_name, int is_header)
{
    if (nplaces >= places_cap) {
        places_cap = places_cap ? places_cap * 2 : 32;
        places = realloc(places, places_cap * sizeof(PlaceEntry));
    }
    PlaceEntry *p = &places[nplaces++];
    p->label = strdup(label);
    p->path = path ? strdup(path) : NULL;
    p->icon_name = icon_name ? strdup(icon_name) : NULL;
    p->is_header = is_header;
}

static void places_free_entries(void)
{
    for (int i = 0; i < nplaces; i++) {
        free(places[i].label);
        free(places[i].path);
        free(places[i].icon_name);
    }
    free(places);
    places = NULL;
    nplaces = 0;
    places_cap = 0;
}

static void sections_free(void)
{
    for (int i = 0; i < nsections; i++)
        free(sections[i].labels);
    free(sections);
    sections = NULL;
    nsections = 0;
}

/* ---------- Build place list ---------- */

static void add_xdg_dir(const char *xdg_name, const char *label,
                         const char *icon_name)
{
    char *path = isde_xdg_user_dir(xdg_name);
    if (path) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            places_add(label, path, icon_name, 0);
        }
        free(path);
    }
}

static void build_places_list(void)
{
    places_free_entries();

    /* --- Places section --- */
    places_add("Places", NULL, NULL, 1);

    const char *home = getenv("HOME");
    if (home)
        places_add("Home", home, "user-home", 0);

    add_xdg_dir("DESKTOP",   "Desktop",   "user-desktop");
    add_xdg_dir("DOCUMENTS", "Documents", "folder-documents");
    add_xdg_dir("DOWNLOAD",  "Downloads", "folder-download");
    add_xdg_dir("MUSIC",     "Music",     "folder-music");
    add_xdg_dir("PICTURES",  "Pictures",  "folder-pictures");
    add_xdg_dir("VIDEOS",    "Videos",    "folder-videos");

    /* Trash */
    char *trash_path = fileops_trash_path();
    places_add("Trash", trash_path, "user-trash", 0);
    free(trash_path);

    /* --- Devices section --- */
    places_add("Devices", NULL, NULL, 1);
    places_add("File System", "/", "drive-harddisk", 0);

    /* Scan mounts for removable/user media */
    FILE *fp = setmntent("/proc/mounts", "r");
    if (fp) {
        struct mntent *me;
        while ((me = getmntent(fp))) {
            /* Show /media and /mnt mounts, skip virtual filesystems */
            if (strncmp(me->mnt_dir, "/media/", 7) == 0 ||
                (strncmp(me->mnt_dir, "/mnt/", 5) == 0 &&
                 strlen(me->mnt_dir) > 5)) {
                /* Use last path component as label */
                const char *name = strrchr(me->mnt_dir, '/');
                name = name ? name + 1 : me->mnt_dir;
                places_add(name, me->mnt_dir, "drive-removable-media", 0);
            }
        }
        endmntent(fp);
    }

    /* --- Bookmarks section --- */
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
                places_add("Bookmarks", NULL, NULL, 1);
                header_added = 1;
            }

            if (!label) {
                label = strrchr(path, '/');
                label = label ? label + 1 : path;
            }
            places_add(label, path, "folder-bookmark", 0);
        }
        fclose(fp);
    }
}

/* ---------- Build sections from flat place list ---------- */

static void build_sections(void)
{
    sections_free();

    /* Count sections (headers) */
    int count = 0;
    for (int i = 0; i < nplaces; i++)
        if (places[i].is_header) count++;

    sections = calloc(count, sizeof(PlaceSection));
    nsections = 0;

    for (int i = 0; i < nplaces; i++) {
        if (!places[i].is_header)
            continue;

        PlaceSection *s = &sections[nsections++];
        s->start_idx = i + 1;

        /* Count items until next header or end */
        s->nitems = 0;
        for (int j = i + 1; j < nplaces && !places[j].is_header; j++)
            s->nitems++;

        /* Build label array for the List widget */
        s->labels = calloc(s->nitems, sizeof(String));
        for (int j = 0; j < s->nitems; j++)
            s->labels[j] = places[s->start_idx + j].label;
    }
}

/* ---------- Sidebar UI ---------- */

static void place_list_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w;
    Fm *fm = (Fm *)cd;
    IswListReturnStruct *ret = (IswListReturnStruct *)call;

    if (ret->list_index == XAW_LIST_NONE)
        return;

    /* Find which section this List widget belongs to */
    for (int i = 0; i < nsections; i++) {
        if (sections[i].list != w)
            continue;

        int idx = sections[i].start_idx + ret->list_index;
        if (idx < nplaces && places[idx].path)
            fm_navigate(fm, places[idx].path);

        /* Unhighlight all other section lists */
        for (int j = 0; j < nsections; j++)
            if (j != i && sections[j].list)
                IswListUnhighlight(sections[j].list);
        return;
    }
}

void places_init(Fm *fm)
{
    build_places_list();
    build_sections();

    /* Create sidebar viewport */
    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNallowVert, True);              n++;
    XtSetArg(args[n], XtNallowHoriz, False);             n++;
    XtSetArg(args[n], XtNuseRight, False);                n++;
    XtSetArg(args[n], XtNborderWidth, 0);                 n++;
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
    for (int i = 0; i < nsections; i++) {
        PlaceSection *s = &sections[i];
        int hdr_idx = s->start_idx - 1;

        /* Section header label */
        n = 0;
        snprintf(wname, sizeof(wname), "placeHdr%d", i);
        XtSetArg(args[n], XtNlabel, places[hdr_idx].label); n++;
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
    (void)fm;
    sections_free();
    places_free_entries();
}
