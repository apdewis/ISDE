/*
 * greeter.h — isde-greeter internal header
 */
#ifndef ISDE_GREETER_H
#define ISDE_GREETER_H

#include <ISW/Intrinsic.h>
#include <ISW/IntrinsicP.h>
#include <ISW/StringDefs.h>
#include <ISW/Shell.h>
#include <ISW/Form.h>
#include <ISW/Label.h>
#include <ISW/Command.h>
#include <ISW/Text.h>
#include <ISW/List.h>
#include <ISW/ISWRender.h>

#include "isde/isde-theme.h"
#include "isde/isde-xdg.h"
#include "isde/isde-config.h"
#include "isde/isde-desktop.h"

/* ---------- Session entry ---------- */
typedef struct {
    char *name;           /* display name */
    char *desktop_file;   /* .desktop filename (e.g. "isde.desktop") */
} GreeterSession;

/* ---------- Greeter state ---------- */
typedef struct Greeter {
    /* Xt core */
    IswAppContext    app;
    Widget          toplevel;
    Widget          shell;         /* OverrideShell (fullscreen) */
    Widget          form;          /* main Form layout */

    /* Clock widgets */
    Widget          clock_time;
    Widget          clock_date;
    char           *clock_time_fmt;
    char           *clock_date_fmt;
    IswIntervalId    clock_timer;

    /* Login form widgets */
    Widget          user_label;
    Widget          user_text;
    Widget          pass_label;
    Widget          pass_text;
    Widget          session_label;
    Widget          session_btn;
    String         *session_names;   /* backing array for dropdown List */
    Widget          error_label;
    Widget          login_btn;

    /* Power buttons */
    Widget          shutdown_btn;
    Widget          reboot_btn;
    Widget          suspend_btn;

    /* Sessions */
    GreeterSession *sessions;
    int             nsessions;
    int             active_session;  /* index into sessions[] */

    /* IPC */
    int             ipc_fd;          /* socket to daemon */
    IswInputId       ipc_input;       /* Xt input handler */
    char            ipc_buf[4096];
    int             ipc_buf_len;

    /* Primary monitor geometry (physical pixels and logical pixels) */
    int             screen_x;
    int             screen_y;
    int             screen_w;
    int             screen_h;
    int             logical_x;
    int             logical_y;
    int             logical_w;
    int             logical_h;

    /* Config */
    char           *color_scheme;    /* colour scheme name from DM config */
    double          scale;           /* explicit scale factor (0 = auto) */
    char           *font_family;     /* greeter font family */
    int             font_size;       /* greeter font size (pt) */
    int             allow_shutdown;
    int             allow_reboot;
    int             allow_suspend;

    /* Mode: 0 = login, 1 = lock screen */
    int             mode_lock;
    char           *lock_user;     /* username for lock mode */

    int             running;
} Greeter;

/* ---------- greeter.c ---------- */
int  greeter_init(Greeter *g, int *argc, char **argv);
void greeter_run(Greeter *g);
void greeter_cleanup(Greeter *g);
void greeter_set_error(Greeter *g, const char *msg);
void greeter_clear_error(Greeter *g);
void greeter_enter_login_mode(Greeter *g);
void greeter_enter_lock_mode(Greeter *g, const char *username);

/* ---------- clock.c ---------- */
void greeter_clock_init(Greeter *g);
void greeter_clock_cleanup(Greeter *g);

/* ---------- sessions.c ---------- */
void greeter_sessions_load(Greeter *g);
void greeter_sessions_cleanup(Greeter *g);

/* ---------- ipc.c ---------- */
int  greeter_ipc_init(Greeter *g);
void greeter_ipc_cleanup(Greeter *g);
int  greeter_ipc_send(Greeter *g, const char *msg);

#endif /* ISDE_GREETER_H */
