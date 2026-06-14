/*
 * session.h — isde-session internal header
 */
#ifndef ISDE_SESSION_H
#define ISDE_SESSION_H

#include "isde-config.h"
#include "isde-dbus.h"
#include "isde-xdg.h"

#include <signal.h>
#include <sys/types.h>
#include <time.h>
#include <xcb/xcb.h>
#include <dbus/dbus.h>

/* Lid close action */
typedef enum LidAction {
    LID_ACTION_SUSPEND,
    LID_ACTION_HIBERNATE,
    LID_ACTION_LOCK,
    LID_ACTION_NOTHING
} LidAction;

/* ---------- Session state ---------- */
typedef struct Session {
    /* Config */
    char       *wm_command;
    char       *panel_command;
    char       *fm_command;

    /* Child process list */
    Child      *children;

    /* Autostart entries from the autostart file */
    char      **autostart_cmds;
    int        *autostart_respawn;
    int         autostart_count;

    /* XCB connection (theme publish, DPMS, screensaver query) */
    xcb_connection_t *conn;
    int               screen_num;

    /* Event loop: self-pipe carrying SIGCHLD notifications into poll() */
    int               sigchld_pipe[2];

    /* Timer deadlines (CLOCK_MONOTONIC, in milliseconds). 0 = disarmed. */
    long long         check_deadline_ms;   /* periodic appearance check */
    long long         idle_deadline_ms;    /* idle lock/suspend check */

    /* Power settings (cached from [power] config) */
    int               idle_suspend_sec;
    int               screen_off_sec;   /* DPMS/screen-saver timeout to restore */
    LidAction         lid_action;

    /* Session settings */
    int               idle_lock_sec;

    /* Confirmation overlay process */
    pid_t             confirm_pid;    /* isde-confirm child, or 0 */

    /* Liveness pipe: parent holds write end, children inherit read end.
     * EOF on read end signals parent death. */
    int               death_pipe[2];

    /* D-Bus */
    IsdeDBus         *dbus;
    DBusConnection   *system_bus;     /* system bus for DM signals */
    DBusConnection   *session_bus;    /* session bus for ScreenSaver inhibit */

    /* Screensaver inhibit (org.freedesktop.ScreenSaver) */
    uint32_t          inhibit_cookies[32];
    char              inhibit_owners[32][64];  /* bus name that holds each cookie */
    int               inhibit_count;
    uint32_t          next_cookie;

    /* Flags set from D-Bus callbacks */
    volatile sig_atomic_t reload_appearance;
    volatile sig_atomic_t reload_display;
    volatile sig_atomic_t reload_power;

    /* Pending confirmation action from ConfirmationRequested signal */
    char              confirm_action[16];

    int         running;
} Session;

/* ---------- session.c ---------- */
int  session_init(Session *s);
void session_run(Session *s);
void session_cleanup(Session *s);
void session_shutdown(Session *s);

void child_kill_all(Session *s);

/* ---------- autostart.c ---------- */
int  autostart_load(Session *s, const char *path);
void autostart_load_xdg(Session *s);
void autostart_free(Session *s);

#endif /* ISDE_SESSION_H */
