#define _POSIX_C_SOURCE 200809L
/*
 * navigate.c — directory navigation and inotify cwd watch
 */
#include "fm.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/inotify.h>
#endif

/* ---------- cwd directory watch ---------- */

#ifdef __linux__
#define CWD_WATCH_MASK (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | \
                        IN_ATTRIB | IN_CLOSE_WRITE | IN_MOVE_SELF | IN_DELETE_SELF)
#define CWD_REFRESH_DEBOUNCE_MS 150

static void cwd_refresh_timer_cb(IswPointer cd, IswIntervalId *id)
{
    (void)id;
    Fm *fm = (Fm *)cd;
    fm->cwd_refresh_timer = 0;
    fm_refresh(fm);
}

static void cwd_watch_cb(IswPointer cd, int *fd, IswInputId *id)
{
    (void)id;
    Fm *fm = (Fm *)cd;

    char buf[4096]
        __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t len;
    int need_refresh = 0;

    while ((len = read(*fd, buf, sizeof(buf))) > 0) {
        for (char *ptr = buf; ptr < buf + len; ) {
            struct inotify_event *ev = (struct inotify_event *)ptr;
            if (ev->mask & (IN_IGNORED | IN_Q_OVERFLOW)) {
                need_refresh = 1;
            } else {
                need_refresh = 1;
            }
            ptr += sizeof(*ev) + ev->len;
        }
    }

    if (need_refresh) {
        if (fm->cwd_refresh_timer) {
            IswRemoveTimeOut(fm->cwd_refresh_timer);
        }
        fm->cwd_refresh_timer = IswAppAddTimeOut(
            fm->app_state->app, CWD_REFRESH_DEBOUNCE_MS,
            cwd_refresh_timer_cb, fm);
    }
}

void cwd_watch_stop(Fm *fm)
{
    if (fm->cwd_refresh_timer) {
        IswRemoveTimeOut(fm->cwd_refresh_timer);
        fm->cwd_refresh_timer = 0;
    }
    if (fm->cwd_input_id) {
        IswRemoveInput(fm->cwd_input_id);
        fm->cwd_input_id = 0;
    }
    if (fm->cwd_inotify_fd >= 0) {
        close(fm->cwd_inotify_fd);
        fm->cwd_inotify_fd = -1;
    }
    fm->cwd_wd = -1;
}

void cwd_watch_start(Fm *fm, const char *path)
{
    cwd_watch_stop(fm);

    fm->cwd_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fm->cwd_inotify_fd < 0) {
        return;
    }

    fm->cwd_wd = inotify_add_watch(fm->cwd_inotify_fd, path, CWD_WATCH_MASK);
    if (fm->cwd_wd < 0) {
        close(fm->cwd_inotify_fd);
        fm->cwd_inotify_fd = -1;
        return;
    }

    fm->cwd_input_id = IswAppAddInput(fm->app_state->app, fm->cwd_inotify_fd,
                                      (IswPointer)IswInputReadMask,
                                      cwd_watch_cb, fm);
}
#else
void cwd_watch_start(Fm *fm, const char *path) { (void)fm; (void)path; }
void cwd_watch_stop(Fm *fm) { (void)fm; }
#endif

/* ---------- navigation ---------- */

void fm_navigate(Fm *fm, const char *path)
{
    fm_dismiss_context(fm);
    thumbs_cancel(fm);

    char *new_path = strdup(path);

    if (browser_read_dir(fm, new_path) != 0) {
        free(new_path);
        return;
    }

    free(fm->cwd);
    fm->cwd = new_path;

    fm->hist_pos++;
    if (fm->hist_pos < FM_HISTORY_MAX) {
        for (int i = fm->hist_pos; i < fm->hist_count; i++) {
            free(fm->history[i]);
        }
        fm->history[fm->hist_pos] = strdup(new_path);
        fm->hist_count = fm->hist_pos + 1;
    }

    cwd_watch_start(fm, fm->cwd);

    fileview_populate(fm);
    navbar_update(fm);
}

void fm_refresh(Fm *fm)
{
    fm_dismiss_context(fm);
    browser_read_dir(fm, fm->cwd);
    fileview_populate(fm);
    navbar_update(fm);
}
