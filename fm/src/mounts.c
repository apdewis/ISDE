#define _POSIX_C_SOURCE 200809L
/*
 * mounts.c — inotify mount monitor for /media/$USER
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/inotify.h>
#endif

#ifdef __linux__
static void mount_changed_cb(IswPointer cd, int *fd, IswInputId *id)
{
    (void)id;
    FmApp *app = (FmApp *)cd;

    char buf[4096]
        __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t len;

    while ((len = read(*fd, buf, sizeof(buf))) > 0) {
        for (char *ptr = buf; ptr < buf + len; ) {
            struct inotify_event *ev = (struct inotify_event *)ptr;
            if (ev->len == 0) {
                ptr += sizeof(*ev) + ev->len;
                continue;
            }

            const char *name = ev->name;
            const char *user = getenv("USER");
            char path[512];
            snprintf(path, sizeof(path), "/media/%s/%s",
                     user ? user : "", name);

            fprintf(stderr, "isde-fm: mount event: %s %s\n",
                    (ev->mask & IN_CREATE) ? "CREATE" : "DELETE", name);

            for (int i = 0; i < app->nwindows; i++) {
                Fm *fm = app->windows[i];
                if (ev->mask & IN_CREATE) {
                    places_device_added(fm, name, path);
                } else if (ev->mask & IN_DELETE) {
                    places_device_removed(fm, name);

                    /* Navigate away if viewing a removed mount */
                    if (strncmp(fm->cwd, path, strlen(path)) == 0) {
                        const char *home = getenv("HOME");
                        fm_navigate(fm, home ? home : "/");
                    }
                }
            }

            ptr += sizeof(*ev) + ev->len;
        }
    }
}

void mount_monitor_init(FmApp *app)
{
    const char *user = getenv("USER");
    if (!user) {
        return;
    }

    char media_path[256];
    snprintf(media_path, sizeof(media_path), "/media/%s", user);

    struct stat st;
    if (stat(media_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return;
    }

    app->mount_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (app->mount_inotify_fd < 0) {
        return;
    }

    app->mount_wd = inotify_add_watch(app->mount_inotify_fd,
                                       media_path,
                                       IN_CREATE | IN_DELETE);
    if (app->mount_wd < 0) {
        close(app->mount_inotify_fd);
        app->mount_inotify_fd = -1;
        return;
    }

    app->mount_input_id = IswAppAddInput(app->app, app->mount_inotify_fd,
                                          (IswPointer)IswInputReadMask,
                                          mount_changed_cb, app);
    fprintf(stderr, "isde-fm: mount monitor watching %s (fd=%d)\n",
            media_path, app->mount_inotify_fd);
}

void mount_monitor_cleanup(FmApp *app)
{
    if (app->mount_inotify_fd >= 0) {
        if (app->mount_input_id) {
            IswRemoveInput(app->mount_input_id);
        }
        close(app->mount_inotify_fd);
        app->mount_inotify_fd = -1;
    }
}
#else
void mount_monitor_init(FmApp *app) { (void)app; }
void mount_monitor_cleanup(FmApp *app) { (void)app; }
#endif
