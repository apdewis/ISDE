#define _POSIX_C_SOURCE 200809L
/*
 * fileops.c — file operations: mkdir, delete, rename
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/wait.h>

int fileops_mkdir(Fm *fm, const char *name)
{
    size_t len = strlen(fm->cwd) + 1 + strlen(name) + 1;
    char *path = malloc(len);
    snprintf(path, len, "%s/%s", fm->cwd, name);

    int ret = mkdir(path, 0755);
    if (ret != 0 && errno == EEXIST) {
        /* Append a number */
        for (int i = 2; i < 100; i++) {
            char numbered[256];
            snprintf(numbered, sizeof(numbered), "%s/%s (%d)",
                     fm->cwd, name, i);
            if (mkdir(numbered, 0755) == 0) {
                free(path);
                return 0;
            }
        }
    }

    free(path);
    return ret;
}

int fileops_delete(Fm *fm, const char *path)
{
    (void)fm;
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;

    if (S_ISDIR(st.st_mode)) {
        /* Use rm -rf via fork/exec for recursive delete */
        pid_t pid = fork();
        if (pid == 0) {
            execlp("rm", "rm", "-rf", path, (char *)NULL);
            _exit(127);
        }
        if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
            return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
        }
        return -1;
    }

    return unlink(path);
}

int fileops_rename(Fm *fm, const char *oldpath, const char *newname)
{
    /* Build new path in the same directory */
    char *dir = strdup(oldpath);
    char *slash = strrchr(dir, '/');
    if (slash)
        *(slash + 1) = '\0';
    else {
        free(dir);
        dir = strdup(fm->cwd);
    }

    size_t len = strlen(dir) + strlen(newname) + 2;
    char *newpath = malloc(len);
    snprintf(newpath, len, "%s/%s", dir, newname);

    int ret = rename(oldpath, newpath);
    free(dir);
    free(newpath);
    return ret;
}
