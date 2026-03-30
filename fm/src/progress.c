#define _POSIX_C_SOURCE 200809L
/*
 * progress.c — progress dialog for background file operations
 *
 * Shows a non-modal dialog with a label, progress bar, and cancel
 * button.  A 200ms timer polls the job's atomic counters to update
 * the bar.  The dialog is not shown immediately — a 500ms delay
 * avoids flashing for fast operations.
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ISW/ProgressBar.h>

#define POLL_INTERVAL_MS  200
#define SHOW_DELAY_MS     500

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

    if (atomic_load(&job->finished)) {
        return;  /* completion handler will clean up */
    }

    int done  = atomic_load(&job->files_done);
    int total = atomic_load(&job->files_total);
    int pct   = (total > 0) ? (done * 100 / total) : 0;
    if (pct > 100) { pct = 100; }

    /* Update progress bar */
    if (job->progress_bar) {
        Arg a;
        XtSetArg(a, XtNvalue, pct);
        XtSetValues(job->progress_bar, &a, 1);
    }

    /* Update label */
    if (job->progress_label) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s %d of %d...",
                 job_type_verb(job->type), done, total);
        Arg a;
        XtSetArg(a, XtNlabel, buf);
        XtSetValues(job->progress_label, &a, 1);
    }

    /* Re-arm timer */
    Fm *win = job->origin_win;
    if (win) {
        job->progress_timer = XtAppAddTimeOut(
            win->app_state->app, POLL_INTERVAL_MS, poll_timer_cb, job);
    }
}

/* ---------- show-delay timer ---------- */

static void create_dialog(FmJob *job)
{
    Fm *win = job->origin_win;
    if (!win) { return; }

    int total = atomic_load(&job->files_total);
    char msg[128];
    snprintf(msg, sizeof(msg), "%s %d item%s...",
             job_type_verb(job->type), total, total == 1 ? "" : "s");

    Arg args[20];
    Cardinal n = 0;
    XtSetArg(args[n], XtNwidth, isde_scale(350));   n++;
    XtSetArg(args[n], XtNheight, isde_scale(120));   n++;
    XtSetArg(args[n], XtNborderWidth, 1);            n++;
    job->progress_shell = XtCreatePopupShell("progressShell",
                                              transientShellWidgetClass,
                                              win->toplevel, args, n);

    /* Vertical layout */
    n = 0;
    XtSetArg(args[n], XtNorientation, XtorientVertical); n++;
    XtSetArg(args[n], XtNborderWidth, 0);                 n++;
    Widget vbox = XtCreateManagedWidget("progressBox", flexBoxWidgetClass,
                                         job->progress_shell, args, n);

    /* Label */
    n = 0;
    XtSetArg(args[n], XtNlabel, msg);                     n++;
    XtSetArg(args[n], XtNborderWidth, 0);                  n++;
    XtSetArg(args[n], XtNjustify, XtJustifyLeft);          n++;
    job->progress_label = XtCreateManagedWidget("progressLabel",
                                                 labelWidgetClass,
                                                 vbox, args, n);

    /* Progress bar */
    n = 0;
    XtSetArg(args[n], XtNvalue, 0);                        n++;
    XtSetArg(args[n], XtNborderWidth, 1);                  n++;
    XtSetArg(args[n], XtNflexGrow, 1);                     n++;
    job->progress_bar = XtCreateManagedWidget("progressBar",
                                               progressBarWidgetClass,
                                               vbox, args, n);

    /* Cancel button */
    n = 0;
    XtSetArg(args[n], XtNlabel, "Cancel");  n++;
    Widget cancel = XtCreateManagedWidget("cancelBtn", commandWidgetClass,
                                           vbox, args, n);
    XtAddCallback(cancel, XtNcallback, cancel_cb, job);

    XtPopup(job->progress_shell, XtGrabNone);
}

static void show_delay_cb(XtPointer closure, XtIntervalId *id)
{
    (void)id;
    FmJob *job = (FmJob *)closure;
    job->show_delay_timer = 0;

    /* If job finished during the delay, don't show anything */
    if (atomic_load(&job->finished)) {
        return;
    }

    create_dialog(job);

    /* Start polling */
    Fm *win = job->origin_win;
    if (win) {
        job->progress_timer = XtAppAddTimeOut(
            win->app_state->app, POLL_INTERVAL_MS, poll_timer_cb, job);
    }
}

/* ---------- public API ---------- */

void progress_start(FmApp *app, FmJob *job)
{
    /* Start the 500ms delay timer — dialog only shows if the job
     * is still running when it fires. */
    job->show_delay_timer = XtAppAddTimeOut(
        app->app, SHOW_DELAY_MS, show_delay_cb, job);
}

void progress_stop(FmJob *job)
{
    if (job->show_delay_timer) {
        XtRemoveTimeOut(job->show_delay_timer);
        job->show_delay_timer = 0;
    }
    if (job->progress_timer) {
        XtRemoveTimeOut(job->progress_timer);
        job->progress_timer = 0;
    }
    if (job->progress_shell) {
        XtPopdown(job->progress_shell);
        XtDestroyWidget(job->progress_shell);
        job->progress_shell = NULL;
        job->progress_bar = NULL;
        job->progress_label = NULL;
    }
}
