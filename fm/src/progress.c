#define _POSIX_C_SOURCE 200809L
/*
 * progress.c — progress dialog for background file operations
 *
 * Uses isde_progress_create/update/destroy from libisde for the dialog
 * UI.  A 200ms timer polls the job's atomic counters to update the bar.
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "isde/isde-dialog.h"

#define POLL_INTERVAL_MS  200

/* ---------- helpers ---------- */

static const char *job_type_verb(FmJobType type)
{
    switch (type) {
    case FM_JOB_COPY:        return "Copying";
    case FM_JOB_MOVE:        return "Moving";
    case FM_JOB_DELETE:      return "Deleting";
    case FM_JOB_TRASH:       return "Trashing";
    case FM_JOB_EMPTY_TRASH: return "Emptying trash";
    }
    return "Processing";
}

/* ---------- cancel button ---------- */

static void cancel_cb(Widget w, XtPointer cd, XtPointer call)
{
    (void)w; (void)call;
    FmJob *job = (FmJob *)cd;
    atomic_store(&job->cancelled, 1);
}

/* ---------- progress poll timer ---------- */

static void poll_timer_cb(XtPointer closure, XtIntervalId *id)
{
    (void)id;
    FmJob *job = (FmJob *)closure;
    job->progress_timer = 0;

    if (atomic_load(&job->finished))
        return;  /* completion handler will clean up */

    int done  = atomic_load(&job->files_done);
    int total = atomic_load(&job->files_total);
    int pct   = (total > 0) ? (done * 100 / total) : 0;
    if (pct > 100) pct = 100;

    char buf[128];
    int cur = done + 1;
    if (cur > total) cur = total;
    snprintf(buf, sizeof(buf), "%s file %d of %d...",
             job_type_verb(job->type), cur, total);

    isde_progress_update(job->progress, pct, buf);

    /* Per-file byte progress (only meaningful for copy/move) */
    if (job->type == FM_JOB_COPY || job->type == FM_JOB_MOVE) {
        long long cb = atomic_load(&job->cur_bytes_done);
        long long ct = atomic_load(&job->cur_bytes_total);
        int fpct = (ct > 0) ? (int)(cb * 100 / ct) : 0;
        if (fpct > 100) fpct = 100;

        char fbuf[128];
        if (ct >= 1024LL * 1024) {
            snprintf(fbuf, sizeof(fbuf), "%.1f / %.1f MB",
                     cb / (1024.0 * 1024.0), ct / (1024.0 * 1024.0));
        } else if (ct >= 1024) {
            snprintf(fbuf, sizeof(fbuf), "%.1f / %.1f KB",
                     cb / 1024.0, ct / 1024.0);
        } else {
            snprintf(fbuf, sizeof(fbuf), "%lld / %lld bytes", cb, ct);
        }
        isde_progress_update_file(job->progress, fpct, fbuf);
    }

    /* Re-arm timer */
    Fm *win = job->origin_win;
    if (win) {
        job->progress_timer = XtAppAddTimeOut(
            win->app_state->app, POLL_INTERVAL_MS, poll_timer_cb, job);
    }
}

/* ---------- public API ---------- */

void progress_start(FmApp *app, FmJob *job)
{
    Fm *win = job->origin_win;
    if (!win) return;

    job->progress = isde_progress_create(win->toplevel,
                                         job_type_verb(job->type),
                                         app->app, cancel_cb, job);

    /* Start polling after the show delay — the timer fires regardless
     * of whether the dialog is visible yet. */
    job->progress_timer = XtAppAddTimeOut(
        app->app, POLL_INTERVAL_MS, poll_timer_cb, job);
}

void progress_stop(FmJob *job)
{
    if (job->progress_timer) {
        XtRemoveTimeOut(job->progress_timer);
        job->progress_timer = 0;
    }
    isde_progress_destroy(job->progress);
    job->progress = NULL;
}
