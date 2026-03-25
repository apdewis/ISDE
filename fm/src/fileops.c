#define _POSIX_C_SOURCE 200809L
/*
 * fileops.c — file operations: copy, mkdir, delete, rename
 *
 * All operations use standard POSIX/C library functions.
 * No fork/exec of external commands.
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

/* ---------- copy a single file ---------- */

static int copy_file(const char *src, const char *dst)
{
    int fd_in = open(src, O_RDONLY);
    if (fd_in < 0) return -1;

    struct stat st;
    fstat(fd_in, &st);

    int fd_out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
    if (fd_out < 0) {
        close(fd_in);
        return -1;
    }

    char buf[65536];
    ssize_t nread;
    while ((nread = read(fd_in, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < nread) {
            ssize_t w = write(fd_out, buf + written, nread - written);
            if (w < 0) {
                close(fd_in);
                close(fd_out);
                return -1;
            }
            written += w;
        }
    }

    close(fd_in);
    close(fd_out);
    return nread < 0 ? -1 : 0;
}

/* ---------- recursive copy ---------- */

static int copy_recursive(const char *src, const char *dst)
{
    struct stat st;
    if (lstat(src, &st) != 0)
        return -1;

    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, st.st_mode) != 0 && errno != EEXIST)
            return -1;

        DIR *dir = opendir(src);
        if (!dir) return -1;

        struct dirent *de;
        int ret = 0;
        while ((de = readdir(dir))) {
            if (strcmp(de->d_name, ".") == 0 ||
                strcmp(de->d_name, "..") == 0)
                continue;

            size_t slen = strlen(src) + 1 + strlen(de->d_name) + 1;
            size_t dlen = strlen(dst) + 1 + strlen(de->d_name) + 1;
            char *child_src = malloc(slen);
            char *child_dst = malloc(dlen);
            snprintf(child_src, slen, "%s/%s", src, de->d_name);
            snprintf(child_dst, dlen, "%s/%s", dst, de->d_name);

            if (copy_recursive(child_src, child_dst) != 0)
                ret = -1;

            free(child_src);
            free(child_dst);
        }
        closedir(dir);
        return ret;

    } else if (S_ISLNK(st.st_mode)) {
        char link_target[PATH_MAX];
        ssize_t len = readlink(src, link_target, sizeof(link_target) - 1);
        if (len < 0) return -1;
        link_target[len] = '\0';
        return symlink(link_target, dst);

    } else {
        return copy_file(src, dst);
    }
}

/* ---------- recursive delete ---------- */

static int delete_recursive(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return -1;

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir) return -1;

        struct dirent *de;
        int ret = 0;
        while ((de = readdir(dir))) {
            if (strcmp(de->d_name, ".") == 0 ||
                strcmp(de->d_name, "..") == 0)
                continue;

            size_t len = strlen(path) + 1 + strlen(de->d_name) + 1;
            char *child = malloc(len);
            snprintf(child, len, "%s/%s", path, de->d_name);

            if (delete_recursive(child) != 0)
                ret = -1;
            free(child);
        }
        closedir(dir);

        if (rmdir(path) != 0)
            ret = -1;
        return ret;

    } else {
        return unlink(path);
    }
}

/* ---------- public API ---------- */

int fileops_copy(const char *src, const char *dst)
{
    return copy_recursive(src, dst);
}

int fileops_mkdir(Fm *fm, const char *name)
{
    size_t len = strlen(fm->cwd) + 1 + strlen(name) + 1;
    char *path = malloc(len);
    snprintf(path, len, "%s/%s", fm->cwd, name);

    int ret = mkdir(path, 0755);
    if (ret != 0 && errno == EEXIST) {
        for (int i = 2; i < 100; i++) {
            char numbered[512];
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
    return delete_recursive(path);
}

int fileops_rename(Fm *fm, const char *oldpath, const char *newname)
{
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
