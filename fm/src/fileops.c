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

/* ---------- conflict resolution ---------- */

/*
 * If dst already exists, generate a unique name:
 *   "file.txt"    -> "file (2).txt",  "file (3).txt", ...
 *   "folder"      -> "folder (2)",    "folder (3)", ...
 *   "file.tar.gz" -> "file (2).tar.gz", ...
 *
 * Returns a malloc'd string (may be the original dst if no conflict).
 * Caller must free the result.
 */
static char *resolve_conflict(const char *dst)
{
    struct stat st;
    if (lstat(dst, &st) != 0)
        return strdup(dst); /* no conflict */

    /* Split into dir, base, extension */
    const char *slash = strrchr(dst, '/');
    const char *base_start = slash ? slash + 1 : dst;
    size_t dir_len = base_start - dst; /* includes trailing / */

    /* Find extension — handle compound extensions like .tar.gz */
    const char *dot = strchr(base_start, '.');
    size_t name_len, ext_len;
    if (dot) {
        name_len = dot - base_start;
        ext_len = strlen(dot);
    } else {
        name_len = strlen(base_start);
        ext_len = 0;
    }

    for (int i = 2; i < 1000; i++) {
        /* dir_len + name_len + " (999)" + ext_len + null */
        size_t total = dir_len + name_len + 8 + ext_len + 1;
        char *candidate = malloc(total);
        if (ext_len > 0) {
            snprintf(candidate, total, "%.*s%.*s (%d)%s",
                     (int)dir_len, dst,
                     (int)name_len, base_start,
                     i, dot);
        } else {
            snprintf(candidate, total, "%.*s%.*s (%d)",
                     (int)dir_len, dst,
                     (int)name_len, base_start,
                     i);
        }
        if (lstat(candidate, &st) != 0)
            return candidate; /* this name is free */
        free(candidate);
    }

    return strdup(dst); /* give up, return original */
}

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

/* ---------- recursive file counting ---------- */

static int count_recursive(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return 0;

    if (!S_ISDIR(st.st_mode))
        return 1;

    int count = 0;
    DIR *dir = opendir(path);
    if (!dir) return 0;

    struct dirent *de;
    while ((de = readdir(dir))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        size_t len = strlen(path) + 1 + strlen(de->d_name) + 1;
        char *child = malloc(len);
        snprintf(child, len, "%s/%s", path, de->d_name);
        count += count_recursive(child);
        free(child);
    }
    closedir(dir);
    return count;
}

/* ---------- progress-aware recursive copy ---------- */

static int copy_recursive_cb(const char *src, const char *dst,
                              atomic_int *done, atomic_int *cancelled)
{
    if (cancelled && atomic_load(cancelled))
        return -1;

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
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            if (cancelled && atomic_load(cancelled)) { ret = -1; break; }

            size_t slen = strlen(src) + 1 + strlen(de->d_name) + 1;
            size_t dlen = strlen(dst) + 1 + strlen(de->d_name) + 1;
            char *child_src = malloc(slen);
            char *child_dst_base = malloc(dlen);
            snprintf(child_src, slen, "%s/%s", src, de->d_name);
            snprintf(child_dst_base, dlen, "%s/%s", dst, de->d_name);

            char *child_dst = resolve_conflict(child_dst_base);
            free(child_dst_base);

            if (copy_recursive_cb(child_src, child_dst, done, cancelled) != 0)
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
        char *resolved = resolve_conflict(dst);
        int ret = symlink(link_target, resolved);
        free(resolved);
        if (done) atomic_fetch_add(done, 1);
        return ret;

    } else {
        int ret = copy_file(src, dst);
        if (done) atomic_fetch_add(done, 1);
        return ret;
    }
}

/* ---------- progress-aware recursive delete ---------- */

static int delete_recursive_cb(const char *path,
                                atomic_int *done, atomic_int *cancelled)
{
    if (cancelled && atomic_load(cancelled))
        return -1;

    struct stat st;
    if (lstat(path, &st) != 0)
        return -1;

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir) return -1;

        struct dirent *de;
        int ret = 0;
        while ((de = readdir(dir))) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            if (cancelled && atomic_load(cancelled)) { ret = -1; break; }

            size_t len = strlen(path) + 1 + strlen(de->d_name) + 1;
            char *child = malloc(len);
            snprintf(child, len, "%s/%s", path, de->d_name);

            if (delete_recursive_cb(child, done, cancelled) != 0)
                ret = -1;
            free(child);
        }
        closedir(dir);

        if (rmdir(path) != 0)
            ret = -1;
        return ret;

    } else {
        int ret = unlink(path);
        if (done) atomic_fetch_add(done, 1);
        return ret;
    }
}

/* ---------- recursive copy (original, no progress) ---------- */

static int copy_recursive(const char *src, const char *dst)
{
    struct stat st;
    if (lstat(src, &st) != 0)
        return -1;

    if (S_ISDIR(st.st_mode)) {
        /* Directories merge — create if missing, recurse into children */
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
            char *child_dst_base = malloc(dlen);
            snprintf(child_src, slen, "%s/%s", src, de->d_name);
            snprintf(child_dst_base, dlen, "%s/%s", dst, de->d_name);

            /* Resolve conflicts for files within merged directories */
            char *child_dst = resolve_conflict(child_dst_base);
            free(child_dst_base);

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
        /* Resolve conflict for symlinks too */
        char *resolved = resolve_conflict(dst);
        int ret = symlink(link_target, resolved);
        free(resolved);
        return ret;

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

/* ---------- trash support (freedesktop.org Trash spec) ---------- */

static char *get_trash_dir(void)
{
    const char *data_home = isde_xdg_data_home();
    size_t len = strlen(data_home) + strlen("/Trash") + 1;
    char *trash = malloc(len);
    snprintf(trash, len, "%s/Trash", data_home);
    return trash;
}

static int ensure_trash_dirs(const char *trash_dir)
{
    char path[PATH_MAX];
    if (mkdir(trash_dir, 0700) != 0 && errno != EEXIST)
        return -1;
    snprintf(path, sizeof(path), "%s/files", trash_dir);
    if (mkdir(path, 0700) != 0 && errno != EEXIST)
        return -1;
    snprintf(path, sizeof(path), "%s/info", trash_dir);
    if (mkdir(path, 0700) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static char *url_encode_path(const char *path)
{
    size_t len = strlen(path);
    char *enc = malloc(len * 3 + 1);
    if (!enc) return NULL;
    char *p = enc;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c == '/' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            *p++ = c;
        } else {
            sprintf(p, "%%%02X", c);
            p += 3;
        }
    }
    *p = '\0';
    return enc;
}

static char *unique_trash_name(const char *trash_dir, const char *basename)
{
    char path[PATH_MAX];
    struct stat st;
    snprintf(path, sizeof(path), "%s/files/%s", trash_dir, basename);
    if (lstat(path, &st) != 0)
        return strdup(basename);

    const char *dot = strchr(basename, '.');
    size_t name_len = dot ? (size_t)(dot - basename) : strlen(basename);
    const char *ext = dot ? dot : "";

    for (int i = 2; i < 10000; i++) {
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof(candidate), "%.*s.%d%s",
                 (int)name_len, basename, i, ext);
        snprintf(path, sizeof(path), "%s/files/%s", trash_dir, candidate);
        if (lstat(path, &st) != 0)
            return strdup(candidate);
    }
    return strdup(basename);
}

int fileops_trash(const char *path)
{
    char *trash_dir = get_trash_dir();
    if (ensure_trash_dirs(trash_dir) != 0) {
        free(trash_dir);
        return -1;
    }

    const char *slash = strrchr(path, '/');
    const char *basename = slash ? slash + 1 : path;
    char *trash_name = unique_trash_name(trash_dir, basename);

    char dst[PATH_MAX];
    snprintf(dst, sizeof(dst), "%s/files/%s", trash_dir, trash_name);

    int ret;
    if (rename(path, dst) == 0) {
        ret = 0;
    } else if (errno == EXDEV) {
        ret = copy_recursive(path, dst);
        if (ret == 0)
            delete_recursive(path);
    } else {
        free(trash_name);
        free(trash_dir);
        return -1;
    }

    if (ret == 0) {
        char info_path[PATH_MAX];
        snprintf(info_path, sizeof(info_path), "%s/info/%s.trashinfo",
                 trash_dir, trash_name);
        FILE *fp = fopen(info_path, "w");
        if (fp) {
            char *encoded = url_encode_path(path);
            time_t now = time(NULL);
            struct tm *tm = localtime(&now);
            char datebuf[32];
            strftime(datebuf, sizeof(datebuf), "%Y-%m-%dT%H:%M:%S", tm);
            fprintf(fp, "[Trash Info]\nPath=%s\nDeletionDate=%s\n",
                    encoded, datebuf);
            fclose(fp);
            free(encoded);
        }
    }

    free(trash_name);
    free(trash_dir);
    return ret;
}

static char *read_trashinfo_path(const char *info_path)
{
    FILE *fp = fopen(info_path, "r");
    if (!fp) return NULL;
    char line[PATH_MAX];
    char *result = NULL;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Path=", 5) == 0) {
            char *val = line + 5;
            char *nl = strchr(val, '\n');
            if (nl) *nl = '\0';
            size_t len = strlen(val);
            result = malloc(len + 1);
            char *out = result;
            for (size_t i = 0; i < len; i++) {
                if (val[i] == '%' && i + 2 < len) {
                    char hex[3] = { val[i+1], val[i+2], '\0' };
                    *out++ = (char)strtol(hex, NULL, 16);
                    i += 2;
                } else {
                    *out++ = val[i];
                }
            }
            *out = '\0';
            break;
        }
    }
    fclose(fp);
    return result;
}

int fileops_restore(const char *trash_name)
{
    char *trash_dir = get_trash_dir();
    char info_path[PATH_MAX];
    snprintf(info_path, sizeof(info_path), "%s/info/%s.trashinfo",
             trash_dir, trash_name);
    char *orig_path = read_trashinfo_path(info_path);
    if (!orig_path) { free(trash_dir); return -1; }

    char *parent = strdup(orig_path);
    char *pslash = strrchr(parent, '/');
    if (pslash && pslash != parent) {
        *pslash = '\0';
        for (char *p = parent + 1; *p; p++) {
            if (*p == '/') { *p = '\0'; mkdir(parent, 0755); *p = '/'; }
        }
        mkdir(parent, 0755);
    }
    free(parent);

    char *dst = resolve_conflict(orig_path);
    char src[PATH_MAX];
    snprintf(src, sizeof(src), "%s/files/%s", trash_dir, trash_name);

    int ret;
    if (rename(src, dst) == 0) {
        ret = 0;
    } else if (errno == EXDEV) {
        ret = copy_recursive(src, dst);
        if (ret == 0) delete_recursive(src);
    } else {
        ret = -1;
    }

    if (ret == 0) unlink(info_path);

    free(orig_path);
    free(dst);
    free(trash_dir);
    return ret;
}

int fileops_empty_trash(void)
{
    char *trash_dir = get_trash_dir();
    char path[PATH_MAX];
    int ret = 0;

    snprintf(path, sizeof(path), "%s/files", trash_dir);
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir))) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            char child[PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
            if (delete_recursive(child) != 0) ret = -1;
        }
        closedir(dir);
    }

    snprintf(path, sizeof(path), "%s/info", trash_dir);
    dir = opendir(path);
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir))) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            char child[PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
            unlink(child);
        }
        closedir(dir);
    }

    free(trash_dir);
    return ret;
}

char *fileops_trash_path(void)
{
    char *trash_dir = get_trash_dir();
    size_t len = strlen(trash_dir) + strlen("/files") + 1;
    char *path = malloc(len);
    snprintf(path, len, "%s/files", trash_dir);
    ensure_trash_dirs(trash_dir);
    free(trash_dir);
    return path;
}

/* ---------- public API ---------- */

int fileops_copy(const char *src, const char *dst)
{
    char *resolved = resolve_conflict(dst);
    int ret = copy_recursive(src, resolved);
    free(resolved);
    return ret;
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

/* ---------- progress-aware public API ---------- */

int fileops_count_files(const char *path)
{
    return count_recursive(path);
}

int fileops_copy_progress(const char *src, const char *dst,
                          atomic_int *done, atomic_int *cancelled)
{
    char *resolved = resolve_conflict(dst);
    int ret = copy_recursive_cb(src, resolved, done, cancelled);
    free(resolved);
    return ret;
}

int fileops_delete_progress(const char *path,
                            atomic_int *done, atomic_int *cancelled)
{
    return delete_recursive_cb(path, done, cancelled);
}
