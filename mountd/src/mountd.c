#define _POSIX_C_SOURCE 200809L
/*
 * mountd.c — isde-mountd initialization, config loading, main event loop
 */
#include "mountd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>

#include "isde/isde-config.h"
#include "isde/isde-xdg.h"

/* ---------- signal self-pipe ---------- */

static int sig_write_fd = -1;

static void sigchld_handler(int sig)
{
    (void)sig;
    char c = 'C';
    if (sig_write_fd >= 0) {
        (void)write(sig_write_fd, &c, 1);
    }
}

static int setup_signal_pipe(MountDaemon *md)
{
    if (pipe(md->sig_pipe) < 0) {
        return -1;
    }
    fcntl(md->sig_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(md->sig_pipe[1], F_SETFL, O_NONBLOCK);
    fcntl(md->sig_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(md->sig_pipe[1], F_SETFD, FD_CLOEXEC);

    sig_write_fd = md->sig_pipe[1];

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    return 0;
}

/* ---------- config loading ---------- */

static void load_config(MountDaemon *md)
{
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (!cfg) {
        return;
    }

    IsdeConfigTable *root  = isde_config_root(cfg);
    IsdeConfigTable *mount = isde_config_table(root, "mount");
    if (mount) {
        const char *base = isde_config_string(mount, "mount_base", NULL);
        if (base) {
            free(md->mount_base);
            md->mount_base = strdup(base);
        }
        md->auto_mount = isde_config_bool(mount, "auto_mount", 0);
    }

    isde_config_free(cfg);
}

/* ---------- public API ---------- */

int mountd_init(MountDaemon *md)
{
    memset(md, 0, sizeof(*md));
    md->plat = mountd_platform_ops();
    md->monitor_fd = -1;
    md->sig_pipe[0] = -1;
    md->sig_pipe[1] = -1;

    /* Defaults */
    md->mount_base = strdup("/media");

    load_config(md);

    if (setup_signal_pipe(md) != 0) {
        fprintf(stderr, "isde-mountd: cannot create signal pipe: %s\n",
                strerror(errno));
        return -1;
    }

    /* D-Bus system bus — non-fatal if unavailable */
    if (mountd_dbus_init(md) != 0) {
        fprintf(stderr, "isde-mountd: D-Bus unavailable\n");
    }

    /* Initialize device monitor */
    if (md->plat->monitor_init(md) != 0) {
        fprintf(stderr, "isde-mountd: cannot initialize device monitor\n");
        return -1;
    }

    /* Enumerate existing devices */
    md->plat->enumerate_devices(md);
    mountd_refresh_mount_state(md);

    md->running = 1;
    return 0;
}

void mountd_run(MountDaemon *md)
{
    while (md->running) {
        struct pollfd pfds[4];
        int nfds = 0;

        /* Signal self-pipe */
        int sig_idx = nfds;
        pfds[nfds].fd = md->sig_pipe[0];
        pfds[nfds].events = POLLIN;
        nfds++;

        /* Device monitor fd */
        int mon_idx = -1;
        if (md->monitor_fd >= 0) {
            mon_idx = nfds;
            pfds[nfds].fd = md->monitor_fd;
            pfds[nfds].events = POLLIN;
            nfds++;
        }

        /* D-Bus system bus fd */
        int dbus_idx = -1;
        int dbus_fd = mountd_dbus_get_fd(md);
        if (dbus_fd >= 0) {
            dbus_idx = nfds;
            pfds[nfds].fd = dbus_fd;
            pfds[nfds].events = POLLIN;
            nfds++;
        }

        int ret = poll(pfds, nfds, 2000);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        /* Handle signals (SIGCHLD) */
        if (pfds[sig_idx].revents & POLLIN) {
            char buf[64];
            while (read(md->sig_pipe[0], buf, sizeof(buf)) > 0) {
                /* drain */
            }
            /* Reap children */
            int status;
            while (waitpid(-1, &status, WNOHANG) > 0) {
                /* nothing to track */
            }
        }

        /* Handle device monitor events */
        if (mon_idx >= 0 && (pfds[mon_idx].revents & POLLIN)) {
            md->plat->monitor_dispatch(md);
        }

        /* Handle D-Bus */
        if (dbus_idx >= 0 && (pfds[dbus_idx].revents & POLLIN)) {
            mountd_dbus_dispatch(md);
        }
    }
}

void mountd_cleanup(MountDaemon *md)
{
    md->plat->monitor_cleanup(md);
    mountd_dbus_cleanup(md);

    if (md->sig_pipe[0] >= 0) { close(md->sig_pipe[0]); }
    if (md->sig_pipe[1] >= 0) { close(md->sig_pipe[1]); }
    sig_write_fd = -1;

    free(md->mount_base);
}
