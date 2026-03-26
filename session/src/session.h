/*
 * session.h — isde-session internal header
 */
#ifndef ISDE_SESSION_H
#define ISDE_SESSION_H

#include "isde/isde-config.h"
#include "isde/isde-dbus.h"
#include "isde/isde-xdg.h"

#include <signal.h>
#include <sys/types.h>

/* ---------- Child process ---------- */
typedef struct Child {
    pid_t       pid;
    char       *command;      /* Original command string */
    int         respawn;      /* 1 if @-prefixed (restart on crash) */
    int         is_wm;        /* 1 if this is the window manager */
    int         is_panel;     /* 1 if this is the panel */
    struct Child *next;
} Child;

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

    /* D-Bus */
    IsdeDBus   *dbus;

    /* Flags set from D-Bus callbacks */
    volatile sig_atomic_t reload_appearance;

    int         running;
} Session;

/* ---------- session.c ---------- */
int  session_init(Session *s);
void session_run(Session *s);
void session_cleanup(Session *s);
void session_shutdown(Session *s);

/* ---------- child.c ---------- */
Child *child_spawn(Session *s, const char *command, int respawn, int is_wm);
void   child_reap(Session *s);
void   child_kill_all(Session *s);
Child *child_find_pid(Session *s, pid_t pid);
void   child_remove(Session *s, Child *c);

/* ---------- autostart.c ---------- */
int  autostart_load(Session *s, const char *path);
void autostart_load_xdg(Session *s);
void autostart_free(Session *s);

#endif /* ISDE_SESSION_H */
