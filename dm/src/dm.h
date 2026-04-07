/*
 * dm.h — isde-dm internal header
 */
#ifndef ISDE_DM_H
#define ISDE_DM_H

#include "platform.h"
#include "isde/isde-config.h"
#include "isde/isde-xdg.h"

#include <sys/types.h>
#include <signal.h>
#include <xcb/xcb.h>
#include <libseat.h>
#include <dbus/dbus.h>

/* ---------- Forward declarations ---------- */

typedef struct Dm Dm;

/* ---------- IPC message limits ---------- */

#define DM_IPC_MAX_MSG    4096
#define DM_IPC_MAX_USER   256
#define DM_IPC_MAX_PASS   1024

/* ---------- Daemon state ---------- */

struct Dm {
    /* Platform ops vtable */
    const DmPlatformOps *plat;

    /* Config */
    char       *greeter_cmd;       /* greeter binary (default: "isde-greeter") */
    char       *default_session;   /* default session .desktop name */
    char       *xserver_cmd;       /* X server binary (default: "/usr/bin/Xorg") */
    char      **xserver_args;      /* extra args for X server (NULL-terminated) */
    int         xserver_nargs;
    int         allow_shutdown;
    int         allow_reboot;
    int         allow_suspend;
    int         dev_mode;          /* 1 = Xephyr mode (no VT, no seat, no root) */

    /* Seat management */
    struct libseat *seat;
    int         seat_fd;           /* pollable fd from libseat */
    int         seat_active;       /* 1 if seat is enabled */
    int         greeter_vt;        /* VT for greeter X server */

    /* X server for greeter */
    pid_t       xserver_pid;
    int         display_num;       /* :0, :1, etc. */
    char        display[16];       /* ":0" string */
    char        xauth_path[256];   /* path to Xauthority file */
    unsigned char xauth_cookie[16];/* MIT-MAGIC-COOKIE-1 */

    /* Greeter process */
    pid_t       greeter_pid;

    /* Active user session */
    pid_t       session_pid;
    char       *session_user;      /* currently logged-in username */
    char       *session_desktop;   /* .desktop file name */
    int         session_vt;        /* VT for user session */

    /* Lock state */
    int         locked;            /* 1 if session is locked */
    int         lock_timeout;      /* idle seconds before auto-lock (0=disabled) */
    time_t      session_active_since; /* monotonic time of last unlock/session start */

    /* IPC socket */
    int         ipc_listen_fd;     /* listening socket */
    int         ipc_client_fd;     /* connected greeter fd (-1 if none) */
    char        ipc_buf[DM_IPC_MAX_MSG]; /* receive buffer */
    int         ipc_buf_len;

    /* D-Bus (system bus) */
    DBusConnection *dbus;

    /* Signal self-pipe */
    int         sig_pipe[2];       /* [0] = read, [1] = write */

    /* Control */
    volatile sig_atomic_t running;
};

/* ---------- dm.c ---------- */
int  dm_init(Dm *dm);
void dm_run(Dm *dm);
void dm_cleanup(Dm *dm);

/* ---------- seat.c ---------- */
int  dm_seat_init(Dm *dm);
void dm_seat_cleanup(Dm *dm);
void dm_seat_dispatch(Dm *dm);

/* ---------- auth.c ---------- */

/* Authenticate a user via PAM.  On success returns 0; on failure returns -1
 * and writes the PAM error message into errbuf. */
int  dm_auth_check(const char *username, const char *password,
                   char *errbuf, size_t errlen);

/* Start a PAM session for the user (pam_open_session + pam_setcred).
 * Called after fork, before exec of the session process.
 * Returns an opaque PAM handle on success, NULL on failure.
 * Caller must pass it to dm_auth_end_session when the session exits. */
void *dm_auth_start_session(const char *username);

/* Close PAM session and free PAM handle. */
void dm_auth_end_session(void *pam_handle);

/* ---------- ipc.c ---------- */
int  dm_ipc_init(Dm *dm);
void dm_ipc_cleanup(Dm *dm);
void dm_ipc_accept(Dm *dm);
void dm_ipc_handle(Dm *dm);
int  dm_ipc_send(Dm *dm, const char *msg);

/* ---------- session.c ---------- */
int  dm_xserver_start(Dm *dm, int vt);
void dm_xserver_stop(Dm *dm);
int  dm_xserver_ready(Dm *dm);
int  dm_session_start(Dm *dm, const char *username, const char *desktop_file);
void dm_session_cleanup(Dm *dm);

/* ---------- greeter_proc.c ---------- */
int  dm_greeter_start(Dm *dm);
void dm_greeter_stop(Dm *dm);

/* ---------- power.c ---------- */
int  dm_power_shutdown(Dm *dm);
int  dm_power_reboot(Dm *dm);
int  dm_power_suspend(Dm *dm);
void dm_kill_sessions(Dm *dm);

/* ---------- dbus.c ---------- */
int  dm_dbus_init(Dm *dm);
void dm_dbus_cleanup(Dm *dm);
void dm_dbus_dispatch(Dm *dm);
int  dm_dbus_get_fd(Dm *dm);
void dm_dbus_emit_session_started(Dm *dm, const char *username,
                                  const char *session);
void dm_dbus_emit_session_ended(Dm *dm, const char *username);
void dm_dbus_emit_locked(Dm *dm);
void dm_dbus_emit_unlocked(Dm *dm);

/* ---------- lock ---------- */
void dm_lock_session(Dm *dm);
void dm_unlock_session(Dm *dm);

#endif /* ISDE_DM_H */
