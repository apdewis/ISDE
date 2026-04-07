#define _POSIX_C_SOURCE 200809L
/*
 * dm.c — isde-dm initialization, config loading, main event loop
 */
#include "dm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

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

static int setup_signal_pipe(Dm *dm)
{
    if (pipe(dm->sig_pipe) < 0) {
        return -1;
    }
    fcntl(dm->sig_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(dm->sig_pipe[1], F_SETFL, O_NONBLOCK);
    fcntl(dm->sig_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(dm->sig_pipe[1], F_SETFD, FD_CLOEXEC);

    sig_write_fd = dm->sig_pipe[1];

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    return 0;
}

/* ---------- config loading ---------- */

static void load_config(Dm *dm)
{
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load(DM_CONFIG_PATH, errbuf, sizeof(errbuf));
    if (!cfg) {
        fprintf(stderr, "isde-dm: %s (using defaults)\n", errbuf);
        return;
    }

    IsdeConfigTable *root = isde_config_root(cfg);

    const char *greeter = isde_config_string(root, "greeter", NULL);
    if (greeter) {
        free(dm->greeter_cmd);
        dm->greeter_cmd = strdup(greeter);
    }
    const char *defsess = isde_config_string(root, "default_session", NULL);
    if (defsess) {
        free(dm->default_session);
        dm->default_session = strdup(defsess);
    }
    dm->allow_shutdown = isde_config_bool(root, "allow_shutdown", 1);
    dm->allow_reboot   = isde_config_bool(root, "allow_reboot", 1);
    dm->allow_suspend  = isde_config_bool(root, "allow_suspend", 1);

    const char *xsrv = isde_config_string(root, "xserver", NULL);
    if (xsrv) {
        free(dm->xserver_cmd);
        dm->xserver_cmd = strdup(xsrv);
    }

    dm->dev_mode = isde_config_bool(root, "dev_mode", 0);
    dm->lock_timeout = isde_config_int(root, "lock_timeout", 0);

    /* In dev mode, default to Xephyr if xserver wasn't explicitly set */
    if (dm->dev_mode && !xsrv) {
        free(dm->xserver_cmd);
        dm->xserver_cmd = strdup("Xephyr");
    }

    IsdeConfigTable *clock = isde_config_table(root, "clock");
    (void)clock; /* greeter reads clock config itself */

    isde_config_free(cfg);
}

/* ---------- runtime directory ---------- */

static int ensure_rundir(Dm *dm)
{
    const char *rundir = dm->plat->rundir;

    if (mkdir(rundir, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "isde-dm: cannot create %s: %s\n",
                rundir, strerror(errno));
        return -1;
    }
    return 0;
}

/* ---------- child reaping ---------- */

static void reap_children(Dm *dm)
{
    /* Drain the signal pipe */
    char buf[64];
    while (read(dm->sig_pipe[0], buf, sizeof(buf)) > 0) {
        /* discard */
    }

    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == dm->xserver_pid) {
            fprintf(stderr, "isde-dm: X server (pid %d) exited\n", pid);
            dm->xserver_pid = 0;
        } else if (pid == dm->greeter_pid) {
            fprintf(stderr, "isde-dm: greeter (pid %d) exited\n", pid);
            dm->greeter_pid = 0;
        } else if (pid == dm->session_pid) {
            fprintf(stderr, "isde-dm: session (pid %d) exited\n", pid);
            dm_session_cleanup(dm);
        }
    }
}

/* ---------- public API ---------- */

int dm_init(Dm *dm)
{
    memset(dm, 0, sizeof(*dm));
    dm->plat = dm_platform_ops();
    dm->ipc_listen_fd = -1;
    dm->ipc_client_fd = -1;
    dm->sig_pipe[0] = -1;
    dm->sig_pipe[1] = -1;
    dm->seat_fd = -1;
    dm->display_num = 0;

    /* Defaults */
    dm->greeter_cmd    = strdup("isde-greeter");
    dm->default_session = strdup("isde.desktop");
    dm->xserver_cmd    = strdup("/usr/bin/Xorg");
    dm->allow_shutdown = 1;
    dm->allow_reboot   = 1;
    dm->allow_suspend  = 1;

    load_config(dm);

    /* Root check — skip in dev mode */
    if (!dm->dev_mode && getuid() != 0) {
        fprintf(stderr, "isde-dm: must be run as root "
                "(or set dev_mode = 1 in [dm])\n");
        return -1;
    }

    if (dm->dev_mode) {
        fprintf(stderr, "isde-dm: dev_mode enabled (Xephyr, no seat, "
                "no root check)\n");
    }

    if (ensure_rundir(dm) != 0) {
        return -1;
    }

    if (setup_signal_pipe(dm) != 0) {
        fprintf(stderr, "isde-dm: cannot create signal pipe: %s\n",
                strerror(errno));
        return -1;
    }

    /* Skip seat management in dev mode */
    if (!dm->dev_mode) {
        if (dm_seat_init(dm) != 0) {
            fprintf(stderr, "isde-dm: cannot open seat\n");
            return -1;
        }
    }

    if (dm_ipc_init(dm) != 0) {
        fprintf(stderr, "isde-dm: cannot create IPC socket\n");
        return -1;
    }

    /* D-Bus system bus — non-fatal if unavailable */
    if (dm_dbus_init(dm) != 0) {
        fprintf(stderr, "isde-dm: D-Bus unavailable, "
                "Lock/Shutdown via D-Bus disabled\n");
    }

    /* Start X server on greeter VT (no VT arg in dev mode) */
    dm->greeter_vt = dm->dev_mode ? 0 : 7;
    if (dm_xserver_start(dm, dm->greeter_vt) != 0) {
        fprintf(stderr, "isde-dm: cannot start X server\n");
        return -1;
    }

    /* Wait for X to be ready */
    if (dm_xserver_ready(dm) != 0) {
        fprintf(stderr, "isde-dm: X server did not become ready\n");
        return -1;
    }

    /* Start greeter */
    if (dm_greeter_start(dm) != 0) {
        fprintf(stderr, "isde-dm: cannot start greeter\n");
        return -1;
    }

    dm->running = 1;
    return 0;
}

void dm_run(Dm *dm)
{
    while (dm->running) {
        struct pollfd pfds[5];
        int nfds = 0;

        /* Signal self-pipe */
        int sig_idx = nfds;
        pfds[nfds].fd = dm->sig_pipe[0];
        pfds[nfds].events = POLLIN;
        nfds++;

        /* libseat fd */
        int seat_idx = -1;
        if (dm->seat_fd >= 0) {
            seat_idx = nfds;
            pfds[nfds].fd = dm->seat_fd;
            pfds[nfds].events = POLLIN;
            nfds++;
        }

        /* IPC listen or client fd */
        int ipc_idx = -1;
        if (dm->ipc_client_fd >= 0) {
            ipc_idx = nfds;
            pfds[nfds].fd = dm->ipc_client_fd;
            pfds[nfds].events = POLLIN;
            nfds++;
        } else if (dm->ipc_listen_fd >= 0) {
            ipc_idx = nfds;
            pfds[nfds].fd = dm->ipc_listen_fd;
            pfds[nfds].events = POLLIN;
            nfds++;
        }

        /* D-Bus system bus fd */
        int dbus_idx = -1;
        int dbus_fd = dm_dbus_get_fd(dm);
        if (dbus_fd >= 0) {
            dbus_idx = nfds;
            pfds[nfds].fd = dbus_fd;
            pfds[nfds].events = POLLIN;
            nfds++;
        }

        int ret = poll(pfds, nfds, 1000);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        /* Handle signals (SIGCHLD) */
        if (pfds[sig_idx].revents & POLLIN) {
            reap_children(dm);
        }

        /* Handle libseat events */
        if (seat_idx >= 0 && (pfds[seat_idx].revents & POLLIN)) {
            dm_seat_dispatch(dm);
        }

        /* Handle IPC */
        if (ipc_idx >= 0 && (pfds[ipc_idx].revents & POLLIN)) {
            if (dm->ipc_client_fd >= 0) {
                dm_ipc_handle(dm);
            } else {
                dm_ipc_accept(dm);
            }
        }

        /* Handle D-Bus */
        if (dbus_idx < 0) {
            static int dbus_warn_once = 0;
            if (!dbus_warn_once) {
                fprintf(stderr, "isde-dm: D-Bus fd not polled (fd=%d)\n",
                        dbus_fd);
                dbus_warn_once = 1;
            }
        } else if (pfds[dbus_idx].revents & POLLIN) {
            dm_dbus_dispatch(dm);
        }

        /* Idle timeout lock check */
        if (dm->lock_timeout > 0 && dm->session_pid > 0 &&
            !dm->locked && dm->session_active_since > 0) {
            time_t now = time(NULL);
            if (now - dm->session_active_since >= dm->lock_timeout) {
                dm_lock_session(dm);
            }
        }

        /* If the greeter died and no session is running, restart it */
        if (dm->greeter_pid == 0 && dm->session_pid == 0 &&
            dm->xserver_pid != 0) {
            fprintf(stderr, "isde-dm: restarting greeter\n");
            dm_greeter_start(dm);
        }

        /* If X server died, restart everything */
        if (dm->xserver_pid == 0 && dm->running) {
            fprintf(stderr, "isde-dm: X server died, restarting\n");
            dm_greeter_stop(dm);
            if (dm_xserver_start(dm, dm->greeter_vt) == 0 &&
                dm_xserver_ready(dm) == 0) {
                dm_greeter_start(dm);
            }
        }
    }
}

void dm_cleanup(Dm *dm)
{
    dm_greeter_stop(dm);
    dm_xserver_stop(dm);
    dm_ipc_cleanup(dm);
    dm_dbus_cleanup(dm);
    dm_seat_cleanup(dm);

    if (dm->sig_pipe[0] >= 0) { close(dm->sig_pipe[0]); }
    if (dm->sig_pipe[1] >= 0) { close(dm->sig_pipe[1]); }
    sig_write_fd = -1;

    free(dm->greeter_cmd);
    free(dm->default_session);
    free(dm->xserver_cmd);
    for (int i = 0; i < dm->xserver_nargs; i++) {
        free(dm->xserver_args[i]);
    }
    free(dm->xserver_args);
    free(dm->session_user);
    free(dm->session_desktop);

    /* Remove runtime directory contents */
    char path[512];
    snprintf(path, sizeof(path), "%s/greeter.sock", dm->plat->rundir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/Xauthority", dm->plat->rundir);
    unlink(path);
}

/* ---------- lock / unlock ---------- */

void dm_lock_session(Dm *dm)
{
    if (dm->locked) {
        return;  /* already locked */
    }
    if (dm->session_pid == 0 || !dm->session_user) {
        fprintf(stderr, "isde-dm: lock ignored — no active session "
                "(pid=%d user=%s)\n",
                (int)dm->session_pid,
                dm->session_user ? dm->session_user : "(null)");
        return;
    }

    fprintf(stderr, "isde-dm: locking session for '%s'\n", dm->session_user);
    dm->locked = 1;

    /* Start greeter in lock mode — dm_greeter_start checks dm->locked
     * and passes --lock <username> to the greeter process. */
    if (dm->greeter_pid == 0) {
        dm_greeter_start(dm);
    }

    /* Switch VT to greeter */
    if (!dm->dev_mode && dm->seat) {
        libseat_switch_session(dm->seat, dm->greeter_vt);
    }

    dm_dbus_emit_locked(dm);
}

void dm_unlock_session(Dm *dm)
{
    if (!dm->locked) {
        return;
    }

    fprintf(stderr, "isde-dm: unlocking session\n");
    dm->locked = 0;
    dm->session_active_since = time(NULL);  /* reset idle timer */

    /* Stop greeter and switch back to session VT */
    dm_greeter_stop(dm);

    if (!dm->dev_mode && dm->seat && dm->session_vt > 0) {
        libseat_switch_session(dm->seat, dm->session_vt);
    }

    dm_dbus_emit_unlocked(dm);
}
