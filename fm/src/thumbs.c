#define _POSIX_C_SOURCE 200809L
/*
 * thumbs.c — thumbnail generation and caching for image/video files
 *
 * Generates PNG thumbnails in ~/.cache/isde/thumbnails/.
 * JPEG decoding via libjpeg-turbo, PNG via Cairo.
 * Video thumbnails via ffmpegthumbnailer (if available at runtime).
 * Thumbnails are generated on a background thread to avoid UI stalls.
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <pthread.h>
#include <limits.h>
#include <sys/wait.h>

#include <cairo/cairo.h>
#include <jpeglib.h>
#include <setjmp.h>

#define THUMB_SIZE 128

/* ------------------------------------------------------------------ */
/* MD5 (RFC 1321) — minimal implementation for cache key hashing      */
/* ------------------------------------------------------------------ */

static const uint32_t md5_T[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,
    0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
    0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,
    0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,
    0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
    0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,
    0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,
    0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
    0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391,
};
static const int md5_s[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21,
};

static inline uint32_t md5_left_rotate(uint32_t x, int c)
{
    return (x << c) | (x >> (32 - c));
}

static void md5_hash(const void *data, size_t len, unsigned char out[16])
{
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89,
             c0 = 0x98badcfe, d0 = 0x10325476;

    size_t padded_len = ((len + 8) / 64 + 1) * 64;
    unsigned char *msg = calloc(padded_len, 1);
    memcpy(msg, data, len);
    msg[len] = 0x80;
    uint64_t bit_len = (uint64_t)len * 8;
    memcpy(msg + padded_len - 8, &bit_len, 8);

    for (size_t offset = 0; offset < padded_len; offset += 64) {
        uint32_t *M = (uint32_t *)(msg + offset);
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; i++) {
            uint32_t F, g;
            if (i < 16)      { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5*i+1) % 16; }
            else if (i < 48) { F = B ^ C ^ D;          g = (3*i+5) % 16; }
            else              { F = C ^ (B | ~D);       g = (7*i) % 16; }
            F += A + md5_T[i] + M[g];
            A = D; D = C; C = B;
            B += md5_left_rotate(F, md5_s[i]);
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
    free(msg);

    memcpy(out,      &a0, 4);
    memcpy(out + 4,  &b0, 4);
    memcpy(out + 8,  &c0, 4);
    memcpy(out + 12, &d0, 4);
}

/* ------------------------------------------------------------------ */
/* Cache path helpers                                                  */
/* ------------------------------------------------------------------ */

static char *cache_dir(void)
{
    const char *cache_home = isde_xdg_cache_home();
    char *dir = NULL;
    if (asprintf(&dir, "%s/isde/thumbnails", cache_home) < 0)
        return NULL;
    return dir;
}

static char *thumb_cache_path(const char *full_path)
{
    unsigned char digest[16];
    md5_hash(full_path, strlen(full_path), digest);

    char hex[33];
    for (int i = 0; i < 16; i++)
        sprintf(hex + i * 2, "%02x", digest[i]);

    char *dir = cache_dir();
    if (!dir) return NULL;

    char *path = NULL;
    if (asprintf(&path, "%s/%s.png", dir, hex) < 0)
        path = NULL;
    free(dir);
    return path;
}

/* Check if cached thumbnail is valid (exists and newer than source). */
static int thumb_cache_valid(const char *cache_path, time_t src_mtime)
{
    struct stat st;
    if (stat(cache_path, &st) != 0)
        return 0;
    return st.st_mtime >= src_mtime;
}

/* ------------------------------------------------------------------ */
/* Image scaling helper — fit into THUMB_SIZE x THUMB_SIZE box         */
/* ------------------------------------------------------------------ */

static int scale_to_png(const unsigned char *rgba, int w, int h,
                        const char *out_path)
{
    if (w <= 0 || h <= 0) return -1;

    /* Compute scaled dimensions preserving aspect ratio */
    int tw, th;
    if (w >= h) {
        tw = THUMB_SIZE;
        th = (int)((double)h / w * THUMB_SIZE);
        if (th < 1) th = 1;
    } else {
        th = THUMB_SIZE;
        tw = (int)((double)w / h * THUMB_SIZE);
        if (tw < 1) tw = 1;
    }

    /* Square canvas with the scaled image centered */
    cairo_surface_t *dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                       THUMB_SIZE, THUMB_SIZE);
    cairo_t *cr = cairo_create(dst);

    int ox = (THUMB_SIZE - tw) / 2;
    int oy = (THUMB_SIZE - th) / 2;

    /* Source surface from RGBA data.
     * Cairo ARGB32 is premultiplied native-endian BGRA on little-endian,
     * so we need to convert from straight RGBA. */
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, w);
    unsigned char *cairo_data = malloc(stride * h);
    for (int y = 0; y < h; y++) {
        const unsigned char *src_row = rgba + y * w * 4;
        uint32_t *dst_row = (uint32_t *)(cairo_data + y * stride);
        for (int x = 0; x < w; x++) {
            unsigned int r = src_row[0], g = src_row[1],
                         b = src_row[2], a = src_row[3];
            /* Premultiply alpha */
            r = r * a / 255;
            g = g * a / 255;
            b = b * a / 255;
            dst_row[x] = (a << 24) | (r << 16) | (g << 8) | b;
            src_row += 4;
        }
    }

    cairo_surface_t *src = cairo_image_surface_create_for_data(
        cairo_data, CAIRO_FORMAT_ARGB32, w, h, stride);

    cairo_translate(cr, ox, oy);
    cairo_scale(cr, (double)tw / w, (double)th / h);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_paint(cr);

    cairo_status_t status = cairo_surface_write_to_png(dst, out_path);

    cairo_destroy(cr);
    cairo_surface_destroy(src);
    cairo_surface_destroy(dst);
    free(cairo_data);

    return (status == CAIRO_STATUS_SUCCESS) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* JPEG decoding via libjpeg-turbo                                     */
/* ------------------------------------------------------------------ */

struct jpeg_error_ctx {
    struct jpeg_error_mgr mgr;
    jmp_buf escape;
};

static void jpeg_error_exit(j_common_ptr cinfo)
{
    struct jpeg_error_ctx *ctx = (struct jpeg_error_ctx *)cinfo->err;
    longjmp(ctx->escape, 1);
}

static int thumb_generate_jpeg(const char *src_path, const char *out_path)
{
    FILE *fp = fopen(src_path, "rb");
    if (!fp) return -1;

    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_ctx jerr;

    cinfo.err = jpeg_std_error(&jerr.mgr);
    jerr.mgr.error_exit = jpeg_error_exit;

    if (setjmp(jerr.escape)) {
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return -1;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);

    cinfo.out_color_space = JCS_RGB;

    /* Use libjpeg's built-in downscaling for large images */
    if (cinfo.image_width > THUMB_SIZE * 4 || cinfo.image_height > THUMB_SIZE * 4)
        cinfo.scale_denom = 4;
    else if (cinfo.image_width > THUMB_SIZE * 2 || cinfo.image_height > THUMB_SIZE * 2)
        cinfo.scale_denom = 2;

    jpeg_start_decompress(&cinfo);

    int w = cinfo.output_width;
    int h = cinfo.output_height;
    int row_stride = w * 3;

    unsigned char *rgba = malloc(w * h * 4);
    unsigned char *row_buf = malloc(row_stride);

    int y = 0;
    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char *rowp = row_buf;
        jpeg_read_scanlines(&cinfo, &rowp, 1);
        unsigned char *dst = rgba + y * w * 4;
        for (int x = 0; x < w; x++) {
            dst[0] = row_buf[x * 3];
            dst[1] = row_buf[x * 3 + 1];
            dst[2] = row_buf[x * 3 + 2];
            dst[3] = 255;
            dst += 4;
        }
        y++;
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);
    free(row_buf);

    int rc = scale_to_png(rgba, w, h, out_path);
    free(rgba);
    return rc;
}

/* ------------------------------------------------------------------ */
/* PNG thumbnail via Cairo                                             */
/* ------------------------------------------------------------------ */

static int thumb_generate_png(const char *src_path, const char *out_path)
{
    cairo_surface_t *src = cairo_image_surface_create_from_png(src_path);
    if (cairo_surface_status(src) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(src);
        return -1;
    }

    int w = cairo_image_surface_get_width(src);
    int h = cairo_image_surface_get_height(src);
    if (w <= 0 || h <= 0) {
        cairo_surface_destroy(src);
        return -1;
    }

    int tw, th;
    if (w >= h) {
        tw = THUMB_SIZE;
        th = (int)((double)h / w * THUMB_SIZE);
        if (th < 1) th = 1;
    } else {
        th = THUMB_SIZE;
        tw = (int)((double)w / h * THUMB_SIZE);
        if (tw < 1) tw = 1;
    }

    cairo_surface_t *dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                       THUMB_SIZE, THUMB_SIZE);
    cairo_t *cr = cairo_create(dst);
    int ox = (THUMB_SIZE - tw) / 2;
    int oy = (THUMB_SIZE - th) / 2;
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, (double)tw / w, (double)th / h);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_paint(cr);

    cairo_status_t status = cairo_surface_write_to_png(dst, out_path);

    cairo_destroy(cr);
    cairo_surface_destroy(src);
    cairo_surface_destroy(dst);

    return (status == CAIRO_STATUS_SUCCESS) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Video thumbnail via ffmpegthumbnailer                               */
/* ------------------------------------------------------------------ */

static int have_ffmpegthumbnailer = -1; /* -1 = unchecked */

static int thumb_generate_video(const char *src_path, const char *out_path)
{
    if (have_ffmpegthumbnailer == -1)
        have_ffmpegthumbnailer = (access("/usr/bin/ffmpegthumbnailer", X_OK) == 0);
    if (!have_ffmpegthumbnailer)
        return -1;

    /* Extract frame to a temp file, then square it like images */
    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", out_path);

    pid_t pid = fork();
    if (pid == 0) {
        char size_str[16];
        snprintf(size_str, sizeof(size_str), "%d", THUMB_SIZE);
        execlp("ffmpegthumbnailer", "ffmpegthumbnailer",
               "-i", src_path, "-o", tmp_path, "-s", size_str,
               "-f", (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return -1;

    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        unlink(tmp_path);
        return -1;
    }

    int rc = thumb_generate_png(tmp_path, out_path);
    unlink(tmp_path);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Format detection                                                    */
/* ------------------------------------------------------------------ */

typedef enum {
    THUMB_FMT_NONE,
    THUMB_FMT_JPEG,
    THUMB_FMT_PNG,
    THUMB_FMT_VIDEO,
} ThumbFormat;

static ThumbFormat detect_format(const char *name)
{
    const char *mime = isde_mime_type_for_file(name);
    if (!mime) return THUMB_FMT_NONE;

    if (strncmp(mime, "image/", 6) == 0) {
        if (strcmp(mime, "image/jpeg") == 0)      return THUMB_FMT_JPEG;
        if (strcmp(mime, "image/png") == 0)        return THUMB_FMT_PNG;
        /* MIME lookup may return base type for compound extensions —
         * fall back to extension check for common image types */
    }
    if (strncmp(mime, "video/", 6) == 0)
        return THUMB_FMT_VIDEO;

    /* Extension fallback for types MIME lookup might miss */
    const char *dot = strrchr(name, '.');
    if (!dot) return THUMB_FMT_NONE;
    dot++;
    if (strcasecmp(dot, "jpg") == 0 || strcasecmp(dot, "jpeg") == 0)
        return THUMB_FMT_JPEG;
    if (strcasecmp(dot, "png") == 0)
        return THUMB_FMT_PNG;

    return THUMB_FMT_NONE;
}

/* ------------------------------------------------------------------ */
/* Synchronous single-file thumbnail generation                        */
/* ------------------------------------------------------------------ */

static int thumb_generate(const char *src_path, const char *cache_path,
                          ThumbFormat fmt)
{
    switch (fmt) {
    case THUMB_FMT_JPEG:  return thumb_generate_jpeg(src_path, cache_path);
    case THUMB_FMT_PNG:   return thumb_generate_png(src_path, cache_path);
    case THUMB_FMT_VIDEO: return thumb_generate_video(src_path, cache_path);
    default:              return -1;
    }
}

/* ------------------------------------------------------------------ */
/* Public: cache lookup                                                */
/* ------------------------------------------------------------------ */

char *thumbs_lookup(const char *full_path, time_t mtime)
{
    char *cp = thumb_cache_path(full_path);
    if (!cp) return NULL;

    if (thumb_cache_valid(cp, mtime))
        return cp;

    free(cp);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Async thumbnail generation                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char       *full_path;
    char       *cache_path;
    char       *name;
    ThumbFormat fmt;
    int         entry_index;
} ThumbRequest;

typedef struct {
    char *cache_path;
    int   entry_index;
} ThumbResult;

typedef struct ThumbJob {
    ThumbRequest *requests;
    int           nrequests;
    ThumbResult  *results;
    int           nresults;
    int           results_cap;
    Fm           *fm;
    pthread_t     thread;
    int           pipe_fd[2]; /* worker writes, main reads */
    IswInputId    input_id;
} ThumbJob;

static void thumb_worker_func(void *arg)
{
    ThumbJob *job = (ThumbJob *)arg;

    for (int i = 0; i < job->nrequests; i++) {
        ThumbRequest *req = &job->requests[i];

        if (thumb_generate(req->full_path, req->cache_path, req->fmt) == 0) {
            if (job->nresults >= job->results_cap) {
                job->results_cap = job->results_cap ? job->results_cap * 2 : 16;
                job->results = realloc(job->results,
                                       job->results_cap * sizeof(ThumbResult));
            }
            job->results[job->nresults].cache_path = req->cache_path;
            job->results[job->nresults].entry_index = req->entry_index;
            job->nresults++;
            req->cache_path = NULL; /* ownership transferred */
        }
    }

    /* Signal main thread */
    char byte = 1;
    (void)write(job->pipe_fd[1], &byte, 1);
}

static void *thumb_thread_entry(void *arg)
{
    thumb_worker_func(arg);
    return NULL;
}

static void thumb_job_free(ThumbJob *job)
{
    if (!job) return;

    for (int i = 0; i < job->nrequests; i++) {
        free(job->requests[i].full_path);
        free(job->requests[i].cache_path);
        free(job->requests[i].name);
    }
    free(job->requests);

    for (int i = 0; i < job->nresults; i++)
        free(job->results[i].cache_path);
    free(job->results);

    if (job->pipe_fd[0] >= 0) close(job->pipe_fd[0]);
    if (job->pipe_fd[1] >= 0) close(job->pipe_fd[1]);

    free(job);
}

static void thumb_done_cb(IswPointer client_data, int *fd, IswInputId *id)
{
    (void)fd; (void)id;
    ThumbJob *job = (ThumbJob *)client_data;
    Fm *fm = job->fm;

    /* Drain the pipe */
    char buf[16];
    (void)read(job->pipe_fd[0], buf, sizeof(buf));

    pthread_join(job->thread, NULL);
    IswRemoveInput(job->input_id);

    /* Apply results: swap icon paths for entries that got thumbnails.
     * Entries may have changed if the user navigated away — validate
     * that the entry index is still in range and the path matches. */
    int changed = 0;
    for (int i = 0; i < job->nresults; i++) {
        ThumbResult *r = &job->results[i];
        if (r->entry_index < 0 || r->entry_index >= fm->nentries)
            continue;
        /* Store the cache path as the entry's thumbnail */
        FmEntry *e = &fm->entries[r->entry_index];

        /* Verify path still matches */
        char *expected = thumb_cache_path(e->full_path);
        if (!expected || strcmp(expected, r->cache_path) != 0) {
            free(expected);
            continue;
        }
        free(expected);

        free(e->thumb_path);
        e->thumb_path = r->cache_path;
        r->cache_path = NULL; /* ownership transferred */
        changed = 1;
    }

    thumb_job_free(job);
    fm->thumb_job = NULL;

    if (changed)
        fileview_populate(fm);
}

void thumbs_apply_cache(Fm *fm)
{
    for (int i = 0; i < fm->nentries; i++) {
        FmEntry *e = &fm->entries[i];
        if (e->is_dir || e->thumb_path) continue;

        ThumbFormat fmt = detect_format(e->name);
        if (fmt == THUMB_FMT_NONE) continue;

        char *cp = thumb_cache_path(e->full_path);
        if (!cp) continue;

        if (thumb_cache_valid(cp, e->mtime))
            e->thumb_path = cp;
        else
            free(cp);
    }
}

void thumbs_populate_async(Fm *fm)
{
    /* Cancel any in-flight job */
    if (fm->thumb_job) {
        ThumbJob *old = fm->thumb_job;
        IswRemoveInput(old->input_id);
        pthread_detach(old->thread);
        fm->thumb_job = NULL;
    }

    ThumbJob *job = calloc(1, sizeof(ThumbJob));
    job->fm = fm;
    job->pipe_fd[0] = job->pipe_fd[1] = -1;

    /* Collect entries that need thumbnails (cache hits already applied) */
    int cap = 0;
    for (int i = 0; i < fm->nentries; i++) {
        FmEntry *e = &fm->entries[i];
        if (e->is_dir || e->thumb_path) continue;

        ThumbFormat fmt = detect_format(e->name);
        if (fmt == THUMB_FMT_NONE) continue;

        char *cp = thumb_cache_path(e->full_path);
        if (!cp) continue;

        if (job->nrequests >= cap) {
            cap = cap ? cap * 2 : 16;
            job->requests = realloc(job->requests, cap * sizeof(ThumbRequest));
        }
        ThumbRequest *req = &job->requests[job->nrequests++];
        req->full_path = strdup(e->full_path);
        req->cache_path = cp;
        req->name = strdup(e->name);
        req->fmt = fmt;
        req->entry_index = i;
    }

    if (job->nrequests == 0) {
        thumb_job_free(job);
        return;
    }

    if (pipe(job->pipe_fd) < 0) {
        thumb_job_free(job);
        return;
    }

    fm->thumb_job = job;
    job->input_id = IswAppAddInput(fm->app_state->app, job->pipe_fd[0],
                                   (IswPointer)IswInputReadMask,
                                   thumb_done_cb, job);

    pthread_create(&job->thread, NULL, thumb_thread_entry, job);
}

/* ------------------------------------------------------------------ */
/* Init / Cleanup                                                      */
/* ------------------------------------------------------------------ */

void thumbs_init(FmApp *app)
{
    (void)app;
    have_ffmpegthumbnailer = -1;

    /* Ensure cache directory exists */
    char *dir = cache_dir();
    if (dir) {
        /* mkdir -p: create parent then child */
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            mkdir(dir, 0755);
            *slash = '/';
        }
        mkdir(dir, 0755);
        free(dir);
    }
}

void thumbs_cleanup(FmApp *app)
{
    (void)app;
}

void thumbs_cancel(Fm *fm)
{
    if (!fm->thumb_job) return;
    ThumbJob *old = fm->thumb_job;
    IswRemoveInput(old->input_id);
    pthread_detach(old->thread);
    fm->thumb_job = NULL;
}
