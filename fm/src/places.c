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

/* ---------- Sidebar UI ---------- */

static void place_click_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    Fm *fm = (Fm *)cd;

    /* Find which button was clicked by matching widget */
    for (int i = 0; i < nplaces; i++) {
        if (places[i].is_header || !places[i].path)
            continue;
        if (fm->place_buttons && fm->place_buttons[i] == w) {
            fm_navigate(fm, places[i].path);
            return;
        }
    }
}

void places_init(Fm *fm)
{
    build_places_list();

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

    /* Vertical FlexBox inside viewport — strict single-column layout */
    n = 0;
    XtSetArg(args[n], XtNorientation, XtorientVertical); n++;
    XtSetArg(args[n], XtNborderWidth, 0);                 n++;
    XtSetArg(args[n], XtNspacing, 0);                     n++;
    fm->places_box = XtCreateManagedWidget("placesBox", flexBoxWidgetClass,
                                            fm->places_vp, args, n);

    /* Allocate button tracking array */
    fm->place_buttons = calloc(nplaces, sizeof(Widget));
    fm->nplace_buttons = nplaces;

    /* Create widgets for each entry */
    char wname[32];
    for (int i = 0; i < nplaces; i++) {
        PlaceEntry *pe = &places[i];

        if (pe->is_header) {
            /* Section header — bold label */
            n = 0;
            snprintf(wname, sizeof(wname), "placeHdr%d", i);
            XtSetArg(args[n], XtNlabel, pe->label);       n++;
            XtSetArg(args[n], XtNborderWidth, 0);          n++;
            XtSetArg(args[n], XtNinternalWidth, isde_scale(6)); n++;
            XtSetArg(args[n], XtNinternalHeight, isde_scale(2)); n++;
            XtSetArg(args[n], XtNjustify, XtJustifyLeft);  n++;
            Widget lbl = XtCreateManagedWidget(wname, labelWidgetClass,
                                                fm->places_box, args, n);
            fm->place_buttons[i] = lbl;
        } else {
            /* Clickable place button */
            n = 0;
            snprintf(wname, sizeof(wname), "place%d", i);
            XtSetArg(args[n], XtNlabel, pe->label);        n++;
            XtSetArg(args[n], XtNborderWidth, 0);           n++;
            XtSetArg(args[n], XtNinternalWidth, isde_scale(4)); n++;
            XtSetArg(args[n], XtNinternalHeight, isde_scale(2)); n++;
            XtSetArg(args[n], XtNjustify, XtJustifyLeft);   n++;

            Widget btn = XtCreateManagedWidget(wname, commandWidgetClass,
                                                fm->places_box, args, n);
            XtAddCallback(btn, XtNcallback, place_click_cb, fm);
            fm->place_buttons[i] = btn;
        }
    }
}

void places_cleanup(Fm *fm)
{
    free(fm->place_buttons);
    fm->place_buttons = NULL;
    fm->nplace_buttons = 0;
    places_free_entries();
}
