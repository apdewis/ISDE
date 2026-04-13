#define _POSIX_C_SOURCE 200809L
/*
 * jobqueue.c — background file operations with worker thread
 *
 * A single worker thread processes file operations (copy, move,
 * delete, trash) from a pending queue.  Finished jobs are moved to
 * a done list.  Completion is signaled to the Xt event loop via a
 * pipe so UI updates happen on the main thread.
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

/* ---------- job helpers ---------- */

static FmJob *job_alloc(FmJobType type, Fm *win,
                        char **srcs, int nsrc, const char *dst_dir)
{
    FmJob *job = calloc(1, sizeof(FmJob));
    if (!job) { return NULL; }
    job->type = type;
    job->origin_win = win;
    job->nsrc = nsrc;
    if (dst_dir) {
        job->dst_dir = strdup(dst_dir);
    }
    if (nsrc > 0 && srcs) {
        job->src_paths = malloc(nsrc * sizeof(char *));
        for (int i = 0; i < nsrc; i++) {
            job->src_paths[i] = strdup(srcs[i]);
        }
    }
    return job;
}

static void job_free(FmJob *job)
{
    for (int i = 0; i < job->nsrc; i++) {
        free(job->src_paths[i]);
    }
    free(job->src_paths);
    free(job->dst_dir);
    free(job);
}

/* Pending queue: main thread enqueues, worker dequeues */
static void enqueue_pending(FmApp *app, FmJob *job)
{
    pthread_mutex_lock(&app->job_mutex);
    if (app->job_tail) {
        app->job_tail->next = job;
    } else {
        app->job_head = job;
    }
    app->job_tail = job;
    pthread_cond_signal(&app->job_cond);
    pthread_mutex_unlock(&app->job_mutex);
}

static FmJob *dequeue_pending(FmApp *app)
{
    pthread_mutex_lock(&app->job_mutex);
    while (!app->job_head && app->worker_running) {
        pthread_cond_wait(&app->job_cond, &app->job_mutex);
    }
    FmJob *job = app->job_head;
    if (job) {
        app->job_head = job->next;
        if (!app->job_head) {
            app->job_tail = NULL;
        }
        job->next = NULL;
    }
    pthread_mutex_unlock(&app->job_mutex);
    return job;
}

/* Done list: worker appends, main thread drains */
static FmJob *done_head;  /* protected by job_mutex */

static void enqueue_done(FmApp *app, FmJob *job)
{
    pthread_mutex_lock(&app->job_mutex);
    job->next = done_head;
    done_head = job;
    pthread_mutex_unlock(&app->job_mutex);
}

static FmJob *drain_done(FmApp *app)
{
    pthread_mutex_lock(&app->job_mutex);
    FmJob *list = done_head;
    done_head = NULL;
    pthread_mutex_unlock(&app->job_mutex);
    return list;
}

/* ---------- file operation execution ---------- */

static void exec_copy(FmJob *job)
{
    /* Pre-count total files for accurate progress */
    int total = 0;
    for (int i = 0; i < job->nsrc; i++) {
        total += fileops_count_files(job->src_paths[i]);
    }
    atomic_store(&job->files_total, total);

    for (int i = 0; i < job->nsrc; i++) {
        if (atomic_load(&job->cancelled)) { break; }

        const char *src = job->src_paths[i];
        const char *base = strrchr(src, '/');
        base = base ? base + 1 : src;

        size_t dlen = strlen(job->dst_dir) + 1 + strlen(base) + 1;
        char *dest = malloc(dlen);
        snprintf(dest, dlen, "%s/%s", job->dst_dir, base);

        int ret = fileops_copy_progress(src, dest,
                                        &job->files_done, &job->cancelled,
                                        &job->cur_bytes_done,
                                        &job->cur_bytes_total);
        free(dest);
        if (ret != 0 && job->error == 0) {
            job->error = errno;
        }
    }
}

static void exec_move(FmJob *job)
{
    /* For move, pre-count in case we need cross-device copy+delete */
    int total = 0;
    for (int i = 0; i < job->nsrc; i++) {
        total += fileops_count_files(job->src_paths[i]);
    }
    atomic_store(&job->files_total, total);

    for (int i = 0; i < job->nsrc; i++) {
        if (atomic_load(&job->cancelled)) { break; }

        const char *src = job->src_paths[i];
        const char *base = strrchr(src, '/');
        base = base ? base + 1 : src;

        size_t dlen = strlen(job->dst_dir) + 1 + strlen(base) + 1;
        char *dest = malloc(dlen);
        snprintf(dest, dlen, "%s/%s", job->dst_dir, base);

        if (strcmp(src, dest) == 0) {
            free(dest);
            /* Count all files in this item as done */
            int n = fileops_count_files(src);
            atomic_fetch_add(&job->files_done, n);
            continue;
        }

        int ret = rename(src, dest);
        if (ret == 0) {
            /* rename is instant — count all files as done */
            int n = fileops_count_files(dest);
            atomic_fetch_add(&job->files_done, n);
        } else if (errno == EXDEV) {
            /* Cross-device: progress-aware copy then delete */
            ret = fileops_copy_progress(src, dest,
                                        &job->files_done, &job->cancelled,
                                        &job->cur_bytes_done,
                                        &job->cur_bytes_total);
            if (ret == 0) {
                fileops_delete(NULL, src);
            }
        }
        if (ret != 0 && job->error == 0) {
            job->error = errno;
        }
        free(dest);
    }
}

static void exec_delete(FmJob *job)
{
    int total = 0;
    for (int i = 0; i < job->nsrc; i++) {
        total += fileops_count_files(job->src_paths[i]);
    }
    atomic_store(&job->files_total, total);

    for (int i = 0; i < job->nsrc; i++) {
        if (atomic_load(&job->cancelled)) { break; }
        int ret = fileops_delete_progress(job->src_paths[i],
                                          &job->files_done, &job->cancelled);
        if (ret != 0 && job->error == 0) {
            job->error = errno;
        }
    }
}

static void exec_trash(FmJob *job)
{
    /* Trash moves whole items — count top-level items, not recursive */
    atomic_store(&job->files_total, job->nsrc);
    for (int i = 0; i < job->nsrc; i++) {
        if (atomic_load(&job->cancelled)) { break; }
        int ret = fileops_trash(job->src_paths[i]);
        if (ret != 0 && job->error == 0) {
            job->error = errno;
        }
        atomic_store(&job->files_done, i + 1);
    }
}

static void exec_empty_trash(FmJob *job)
{
    atomic_store(&job->files_total, 1);
    int ret = fileops_empty_trash();
    if (ret != 0 && job->error == 0) {
        job->error = errno;
    }
    atomic_store(&job->files_done, 1);
}

/* ---------- worker thread ---------- */

static void *worker_func(void *arg)
{
    FmApp *app = (FmApp *)arg;

    while (1) {
        FmJob *job = dequeue_pending(app);
        if (!job) { break; }  /* shutdown sentinel */

        switch (job->type) {
        case FM_JOB_COPY:        exec_copy(job);        break;
        case FM_JOB_MOVE:        exec_move(job);        break;
        case FM_JOB_DELETE:      exec_delete(job);      break;
        case FM_JOB_TRASH:       exec_trash(job);       break;
        case FM_JOB_EMPTY_TRASH: exec_empty_trash(job); break;
        }

        atomic_store(&job->finished, 1);

        /* Move to done list and wake the main thread */
        enqueue_done(app, job);
        char c = 1;
        (void)write(app->notify_pipe[1], &c, 1);
    }

    return NULL;
}

/* ---------- main-thread completion handler ---------- */

static void notify_cb(XtPointer closure, int *fd, XtInputId *id)
{
    (void)fd; (void)id;
    FmApp *app = (FmApp *)closure;

    /* Drain the pipe */
    char buf[64];
    (void)read(app->notify_pipe[0], buf, sizeof(buf));

    /* Collect all finished jobs */
    FmJob *list = drain_done(app);
    while (list) {
        FmJob *job = list;
        list = job->next;
        job->next = NULL;

        /* Dismiss progress dialog */
        progress_stop(job);

        /* Refresh the origin window if it's still open */
        Fm *win = job->origin_win;
        for (int i = 0; i < app->nwindows; i++) {
            if (app->windows[i] == win) {
                fm_refresh(win);
                break;
            }
        }

        if (job->error) {
            fprintf(stderr, "isde-fm: file operation failed: %s\n",
                    strerror(job->error));
        }
        if (atomic_load(&job->cancelled)) {
            fprintf(stderr, "isde-fm: file operation cancelled\n");
        }

        job_free(job);
    }
}

/* ---------- public API ---------- */

void jobqueue_init(FmApp *app)
{
    pipe(app->notify_pipe);
    fcntl(app->notify_pipe[0], F_SETFL,
          fcntl(app->notify_pipe[0], F_GETFL) | O_NONBLOCK);

    pthread_mutex_init(&app->job_mutex, NULL);
    pthread_cond_init(&app->job_cond, NULL);
    app->worker_running = 1;
    done_head = NULL;

    app->notify_input_id = XtAppAddInput(app->app, app->notify_pipe[0],
                                          (XtPointer)XtInputReadMask,
                                          notify_cb, app);

    pthread_create(&app->worker_thread, NULL, worker_func, app);
}

void jobqueue_shutdown(FmApp *app)
{
    /* Signal worker to exit */
    pthread_mutex_lock(&app->job_mutex);
    app->worker_running = 0;
    pthread_cond_signal(&app->job_cond);
    pthread_mutex_unlock(&app->job_mutex);

    pthread_join(app->worker_thread, NULL);

    /* Free any remaining pending jobs */
    FmJob *job = app->job_head;
    while (job) {
        FmJob *next = job->next;
        progress_stop(job);
        job_free(job);
        job = next;
    }
    app->job_head = app->job_tail = NULL;

    /* Free any remaining done jobs */
    job = done_head;
    while (job) {
        FmJob *next = job->next;
        progress_stop(job);
        job_free(job);
        job = next;
    }
    done_head = NULL;

    if (app->notify_input_id) {
        XtRemoveInput(app->notify_input_id);
    }
    close(app->notify_pipe[0]);
    close(app->notify_pipe[1]);

    pthread_mutex_destroy(&app->job_mutex);
    pthread_cond_destroy(&app->job_cond);
}

FmJob *jobqueue_submit_copy(FmApp *app, Fm *win,
                            char **srcs, int nsrc, const char *dst_dir)
{
    FmJob *job = job_alloc(FM_JOB_COPY, win, srcs, nsrc, dst_dir);
    if (job) { enqueue_pending(app, job); progress_start(app, job); }
    return job;
}

FmJob *jobqueue_submit_move(FmApp *app, Fm *win,
                            char **srcs, int nsrc, const char *dst_dir)
{
    FmJob *job = job_alloc(FM_JOB_MOVE, win, srcs, nsrc, dst_dir);
    if (job) { enqueue_pending(app, job); progress_start(app, job); }
    return job;
}

FmJob *jobqueue_submit_delete(FmApp *app, Fm *win,
                              char **srcs, int nsrc)
{
    FmJob *job = job_alloc(FM_JOB_DELETE, win, srcs, nsrc, NULL);
    if (job) { enqueue_pending(app, job); progress_start(app, job); }
    return job;
}

FmJob *jobqueue_submit_trash(FmApp *app, Fm *win,
                             char **srcs, int nsrc)
{
    FmJob *job = job_alloc(FM_JOB_TRASH, win, srcs, nsrc, NULL);
    if (job) { enqueue_pending(app, job); progress_start(app, job); }
    return job;
}

FmJob *jobqueue_submit_empty_trash(FmApp *app, Fm *win)
{
    FmJob *job = job_alloc(FM_JOB_EMPTY_TRASH, win, NULL, 0, NULL);
    if (job) { enqueue_pending(app, job); progress_start(app, job); }
    return job;
}
