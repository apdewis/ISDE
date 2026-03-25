#define _POSIX_C_SOURCE 200809L
/*
 * browser.c — directory reading, sorting, file metadata
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int entry_cmp(const void *a, const void *b)
{
    const FmEntry *ea = (const FmEntry *)a;
    const FmEntry *eb = (const FmEntry *)b;

    /* Directories first */
    if (ea->is_dir != eb->is_dir)
        return eb->is_dir - ea->is_dir;

    /* Then alphabetical, case-insensitive */
    return strcasecmp(ea->name, eb->name);
}

void browser_free_entries(Fm *fm)
{
    for (int i = 0; i < fm->nentries; i++) {
        free(fm->entries[i].name);
        free(fm->entries[i].full_path);
    }
    free(fm->entries);
    fm->entries = NULL;
    fm->nentries = 0;
}

int browser_read_dir(Fm *fm, const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "isde-fm: cannot open %s\n", path);
        return -1;
    }

    browser_free_entries(fm);

    int cap = 64;
    fm->entries = calloc(cap, sizeof(FmEntry));
    fm->nentries = 0;

    struct dirent *de;
    while ((de = readdir(dir))) {
        /* Skip . and .. */
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        int hidden = (de->d_name[0] == '.');
        if (hidden && !fm->show_hidden)
            continue;

        if (fm->nentries >= cap) {
            cap *= 2;
            fm->entries = realloc(fm->entries, cap * sizeof(FmEntry));
        }

        FmEntry *e = &fm->entries[fm->nentries];
        memset(e, 0, sizeof(*e));
        e->name = strdup(de->d_name);
        e->is_hidden = hidden;

        /* Build full path */
        size_t plen = strlen(path);
        size_t nlen = strlen(de->d_name);
        e->full_path = malloc(plen + 1 + nlen + 1);
        if (plen > 1)
            snprintf(e->full_path, plen + 1 + nlen + 1, "%s/%s",
                     path, de->d_name);
        else
            snprintf(e->full_path, plen + 1 + nlen + 1, "/%s", de->d_name);

        /* Stat for metadata */
        struct stat st;
        if (stat(e->full_path, &st) == 0) {
            e->mode  = st.st_mode;
            e->size  = st.st_size;
            e->mtime = st.st_mtime;
            e->is_dir = S_ISDIR(st.st_mode);
        }

        e->mime_icon = icons_for_entry(e);
        fm->nentries++;
    }

    closedir(dir);

    /* Sort: dirs first, then alphabetical */
    qsort(fm->entries, fm->nentries, sizeof(FmEntry), entry_cmp);

    return 0;
}

void browser_open_entry(Fm *fm, int index)
{
    if (index < 0 || index >= fm->nentries)
        return;

    FmEntry *e = &fm->entries[index];

    if (e->is_dir) {
        fm_navigate(fm, e->full_path);
    } else if (e->mode & S_IXUSR) {
        /* Executable — launch directly */
        pid_t pid = fork();
        if (pid == 0) {
            chdir(fm->cwd);
            execl(e->full_path, e->name, (char *)NULL);
            _exit(127);
        }
    } else {
        /* Try xdg-open */
        pid_t pid = fork();
        if (pid == 0) {
            execlp("xdg-open", "xdg-open", e->full_path, (char *)NULL);
            _exit(127);
        }
    }
}
