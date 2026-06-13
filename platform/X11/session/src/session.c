#define _POSIX_C_SOURCE 200809L
/*
 * session.c — session initialization, config loading, startup sequence, main loop
 */
#include "../../../common/session/session.h"
#include "../../../common/session/child.h"
#include "../../common/ewmh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <xcb/xcb.h>
#include <xcb/xkb.h>
#include <xcb/randr.h>
#include <xcb/dpms.h>
#include <xcb/screensaver.h>
#include <dbus/dbus.h>

#include "isde/isde-theme.h"
#include "isde/isde-dpms.h"

/* ---------- monotonic clock helper ---------- */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------- apply input settings from config ---------- */

static void apply_appearance_settings(void)
{
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (!cfg) { return; }

    IsdeConfigTable *root = isde_config_root(cfg);
    IsdeConfigTable *appear = isde_config_table(root, "appearance");
    if (appear) {
        const char *cursor = isde_config_string(appear, "cursor_theme", NULL);
        if (cursor) {
            setenv("XCURSOR_THEME", cursor, 1);
            fprintf(stderr, "isde-session: cursor theme=%s\n", cursor);
        }
        const char *cursor_size = isde_config_string(appear, "cursor_size", NULL);
        if (cursor_size) {
            setenv("XCURSOR_SIZE", cursor_size, 1);
        }
    }

    /* Set scaling environment variables for HiDPI */
    IsdeConfigTable *disp = isde_config_table(root, "display");
    if (disp) {
        int scale = (int)isde_config_int(disp, "scale_percent", 100);
        if (scale > 0) {
            char buf[32];

            /* ISW (libISW toolkit) */
            snprintf(buf, sizeof(buf), "%.2f", scale / 100.0);
            setenv("ISW_SCALE_FACTOR", buf, 1);

            /* GDK (GTK 3/4): integer scale + fractional DPI correction */
            int gdk_scale = scale / 100;
            if (gdk_scale < 1) { gdk_scale = 1; }
            snprintf(buf, sizeof(buf), "%d", gdk_scale);
            setenv("GDK_SCALE", buf, 1);
            snprintf(buf, sizeof(buf), "%.4f", scale / (gdk_scale * 100.0));
            setenv("GDK_DPI_SCALE", buf, 1);

            /* Qt 5/6 */
            snprintf(buf, sizeof(buf), "%.2f", scale / 100.0);
            setenv("QT_SCALE_FACTOR", buf, 1);

            fprintf(stderr,
                    "isde-session: scale=%d%% ISW=%.2f GDK_SCALE=%d "
                    "GDK_DPI_SCALE=%.4f QT_SCALE_FACTOR=%.2f\n",
                    scale, scale / 100.0, gdk_scale,
                    scale / (gdk_scale * 100.0), scale / 100.0);
        }
    }

    isde_config_free(cfg);
}


static void apply_input_settings(void)
{
    char errbuf[256];

    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (!cfg) { return; }

    IsdeConfigTable *root = isde_config_root(cfg);
    IsdeConfigTable *input = isde_config_table(root, "input");
    if (!input) { isde_config_free(cfg); return; }

    int accel     = (int)isde_config_int(input, "mouse_acceleration", -1);
    int threshold = (int)isde_config_int(input, "mouse_threshold", -1);

    IsdeConfigTable *kb = isde_config_table(root, "keyboard");
    int rep_delay = kb ? (int)isde_config_int(kb, "repeat_delay", -1) : -1;
    int rep_int   = kb ? (int)isde_config_int(kb, "repeat_interval", -1) : -1;
    isde_config_free(cfg);

    /* Connect to X to apply settings */
    const char *display = getenv("DISPLAY");
    int screen;
    xcb_connection_t *conn = xcb_connect(display, &screen);
    if (xcb_connection_has_error(conn)) {
        xcb_disconnect(conn);
        return;
    }

    /* Mouse acceleration and threshold */
    if (accel > 0 || threshold > 0) {
        xcb_change_pointer_control(conn,
            accel > 0 ? accel : 2,   /* numerator */
            1,                        /* denominator */
            threshold > 0 ? threshold : 4,
            accel > 0,
            threshold > 0);
        fprintf(stderr, "isde-session: mouse accel=%d threshold=%d\n",
                accel, threshold);
    }

    /* Keyboard repeat */
    if (rep_delay > 0 || rep_int > 0) {
        /* Initialize XKB extension */
        xcb_xkb_use_extension(conn, 1, 0);

        uint8_t per_key_repeat[32];
        memset(per_key_repeat, 0xFF, sizeof(per_key_repeat));

        xcb_xkb_set_controls(conn,
            XCB_XKB_ID_USE_CORE_KBD,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            rep_delay > 0 ? rep_delay : 300,
            rep_int > 0 ? rep_int : 30,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            XCB_XKB_BOOL_CTRL_REPEAT_KEYS,
            0,
            per_key_repeat);
        fprintf(stderr, "isde-session: keyboard repeat delay=%d interval=%d\n",
                rep_delay, rep_int);
    }

    xcb_flush(conn);
    xcb_disconnect(conn);
}

/* ---------- apply power settings from config ---------- */

static LidAction parse_lid_action(const char *s)
{
    if (strcmp(s, "hibernate") == 0) { return LID_ACTION_HIBERNATE; }
    if (strcmp(s, "lock") == 0)      { return LID_ACTION_LOCK; }
    if (strcmp(s, "nothing") == 0)   { return LID_ACTION_NOTHING; }
    return LID_ACTION_SUSPEND;
}

/* Apply screen-blanking state based on active inhibitors.  With one or more
 * inhibitors held (e.g. a browser playing video) both DPMS and the X built-in
 * screen saver are disabled; otherwise the configured screen_off timeout is
 * restored. */
void update_blanking_inhibit(Session *s)
{
    xcb_connection_t *conn = (xcb_connection_t *)s->server_context;
    if (!conn) {
        return;
    }

    if (s->inhibit_count > 0) {
        isde_dpms_set_timeouts(conn, 0, 0, 0);
        xcb_set_screen_saver(conn, 0, 0,
                             XCB_BLANKING_DEFAULT, XCB_EXPOSURES_DEFAULT);
    } else {
        isde_dpms_set_timeouts(conn, s->screen_off_sec,
                               s->screen_off_sec, s->screen_off_sec);
        xcb_set_screen_saver(conn, -1, 0,
                             XCB_BLANKING_DEFAULT, XCB_EXPOSURES_DEFAULT);
    }
    xcb_flush(conn);
}

void apply_power_settings(Session *s)
{
    char errbuf[256];
    xcb_connection_t *conn = (xcb_connection_t *)s->server_context;
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (!cfg) { return; }

    IsdeConfigTable *root = isde_config_root(cfg);
    IsdeConfigTable *power = isde_config_table(root, "power");
    if (!power) { isde_config_free(cfg); return; }

    int screen_off = (int)isde_config_int(power, "screen_off_sec", 600);
    s->idle_suspend_sec = (int)isde_config_int(power, "idle_suspend_sec", 0);
    const char *la = isde_config_string(power, "lid_action", "suspend");
    s->lid_action = parse_lid_action(la);

    isde_config_free(cfg);

    /* Apply DPMS timeouts on the session's XCB connection */
    if (conn) {
        isde_dpms_set_timeouts(conn, screen_off, screen_off, screen_off);
    }

    fprintf(stderr, "isde-session: power: screen_off=%d idle_suspend=%d "
            "lid_action=%d\n", screen_off, s->idle_suspend_sec,
            (int)s->lid_action);
}

static const DBusObjectPathVTable screensaver_vtable = {
    .message_function = screensaver_message_handler
};

/* ---------- idle suspend timer ---------- */

/* Resolve the root window of the session's XCB screen. */
static xcb_window_t session_root(Session *s)
{
    xcb_connection_t *conn = (xcb_connection_t *)s->server_context;
    xcb_screen_iterator_t it =
        xcb_setup_roots_iterator(xcb_get_setup(conn));
    for (int i = 0; i < s->screen_num; i++) { xcb_screen_next(&it); }
    return it.data->root;
}

static void idle_timer_fire(Session *s)
{
    xcb_connection_t *conn = (xcb_connection_t *)s->server_context;
    s->idle_deadline_ms = now_ms() + 10000;  /* re-arm: poll every 10s */

    if (s->inhibit_count > 0) {
        return;
    }

    if ((s->idle_lock_sec > 0 || s->idle_suspend_sec > 0) && conn) {
        xcb_screensaver_query_info_cookie_t cookie =
            xcb_screensaver_query_info(conn, session_root(s));
        xcb_screensaver_query_info_reply_t *reply =
            xcb_screensaver_query_info_reply(conn, cookie, NULL);
        if (reply) {
            int idle_sec = reply->ms_since_user_input / 1000;
            free(reply);
            if (s->idle_lock_sec > 0 && idle_sec >= s->idle_lock_sec) {
                dm_dbus_call("Lock");
            }
            if (s->idle_suspend_sec > 0 && idle_sec >= s->idle_suspend_sec) {
                fprintf(stderr, "isde-session: idle %ds >= %ds, suspending\n",
                        idle_sec, s->idle_suspend_sec);
                dm_dbus_call("Suspend");
            }
        }
    }
}

/* ---------- D-Bus settings callback ---------- */

static void on_settings_changed(const char *section, const char *key,
                                void *user_data)
{
    (void)key;
    Session *s = (Session *)user_data;
    if (strcmp(section, "display") == 0 ||
        strcmp(section, "*") == 0) {
        s->reload_display = 1;
    }
    if (strcmp(section, "appearance") == 0 ||
        strcmp(section, "*") == 0) {
        s->reload_appearance = 1;
    }
    if (strcmp(section, "power") == 0 ||
        strcmp(section, "*") == 0) {
        s->reload_power = 1;
    }
}

/* ---------- SIGCHLD ---------- */

/* Write end of the self-pipe used to wake poll() from the signal handler.
 * -1 until the pipe is created in session_init(). */
static volatile sig_atomic_t sigchld_write_fd = -1;

static void sigchld_handler(int sig)
{
    (void)sig;
    int fd = sigchld_write_fd;
    if (fd >= 0) {
        char b = 0;
        ssize_t n = write(fd, &b, 1);
        (void)n;  /* a full pipe already has a pending wakeup byte */
    }
}

static void confirm_reap(Session *s)
{
    if (s->confirm_pid <= 0) {
        return;
    }

    int status;
    pid_t pid = waitpid(s->confirm_pid, &status, WNOHANG);
    if (pid <= 0) {
        return;
    }

    s->confirm_pid = 0;

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "isde-session: confirm '%s' accepted\n",
                s->confirm_action);
        if (strcmp(s->confirm_action, "shutdown") == 0) {
            dm_dbus_call("Shutdown");
        } else if (strcmp(s->confirm_action, "reboot") == 0) {
            dm_dbus_call("Reboot");
        } else if (strcmp(s->confirm_action, "suspend") == 0) {
            dm_dbus_call("Suspend");
        } else if (strcmp(s->confirm_action, "logout") == 0) {
            s->running = 0;
        }
    } else {
        fprintf(stderr, "isde-session: confirm '%s' cancelled\n",
                s->confirm_action);
    }
    s->confirm_action[0] = '\0';
}

static void sigchld_dispatch(Session *s)
{
    confirm_reap(s);
    child_reap(s);
}

/* ---------- Confirmation overlay (separate process) ---------- */

static void show_confirmation(Session *s, const char *action)
{
    fprintf(stderr, "isde-session: show_confirmation(%s)\n", action);

    if (s->confirm_pid > 0) {
        return;  /* already showing */
    }

    if (strcmp(action, "shutdown") != 0 &&
        strcmp(action, "reboot") != 0 &&
        strcmp(action, "suspend") != 0 &&
        strcmp(action, "logout") != 0) {
        return;
    }

    snprintf(s->confirm_action, sizeof(s->confirm_action), "%s", action);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "isde-session: fork isde-confirm: %s\n",
                strerror(errno));
        return;
    }

    if (pid == 0) {
        execlp("isde-confirm", "isde-confirm",
               "--action", action, (char *)NULL);
        fprintf(stderr, "isde-session: exec isde-confirm: %s\n",
                strerror(errno));
        _exit(1);
    }

    s->confirm_pid = pid;
    fprintf(stderr, "isde-session: isde-confirm started (pid %d)\n", pid);
}

/* ---------- org.isde.Session command service ---------- */

static DBusHandlerResult
session_command_handler(DBusConnection *conn, DBusMessage *msg,
                        void *user_data)
{
    (void)conn;
    Session *s = (Session *)user_data;

    if (dbus_message_is_method_call(msg, ISDE_SESSION_DBUS_INTERFACE,
                                    "Logout")) {
        fprintf(stderr, "isde-session: logout requested\n");
        show_confirmation(s, "logout");
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, ISDE_SESSION_DBUS_INTERFACE,
                                    "Shutdown")) {
        fprintf(stderr, "isde-session: shutdown requested\n");
        show_confirmation(s, "shutdown");
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, ISDE_SESSION_DBUS_INTERFACE,
                                    "Reboot")) {
        fprintf(stderr, "isde-session: reboot requested\n");
        show_confirmation(s, "reboot");
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, ISDE_SESSION_DBUS_INTERFACE,
                                    "Suspend")) {
        fprintf(stderr, "isde-session: suspend requested\n");
        show_confirmation(s, "suspend");
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, ISDE_SESSION_DBUS_INTERFACE,
                                    "Lock")) {
        fprintf(stderr, "isde-session: lock requested\n");
        dm_dbus_call("Lock");
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static const DBusObjectPathVTable session_command_vtable = {
    .message_function = session_command_handler
};

/* Register the org.isde.Session object on the already-open session bus
 * (shared with the screensaver inhibit service). */
static void init_session_command_service(Session *s)
{
    if (!s->session_bus) {
        return;
    }

    DBusError err;
    dbus_error_init(&err);
    int ret = dbus_bus_request_name(s->session_bus, ISDE_SESSION_DBUS_SERVICE,
                                    DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "isde-session: cannot own %s: %s\n",
                ISDE_SESSION_DBUS_SERVICE,
                dbus_error_is_set(&err) ? err.message : "already owned");
        dbus_error_free(&err);
        return;
    }
    dbus_error_free(&err);

    dbus_connection_register_object_path(s->session_bus,
                                         ISDE_SESSION_DBUS_PATH,
                                         &session_command_vtable, s);
    fprintf(stderr, "isde-session: command service active\n");
}

/* ---------- System bus: ConfirmationRequested signal ---------- */

static DBusHandlerResult
system_bus_filter(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    (void)conn;
    Session *s = (Session *)user_data;

    if (dbus_message_is_signal(msg, "org.isde.DisplayManager",
                               "ConfirmationRequested")) {
        const char *action = NULL;
        if (dbus_message_get_args(msg, NULL,
                                  DBUS_TYPE_STRING, &action,
                                  DBUS_TYPE_INVALID) && action) {
            show_confirmation(s, action);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_signal(msg, "org.isde.DisplayManager",
                               "LidSwitchChanged")) {
        dbus_bool_t closed = FALSE;
        if (dbus_message_get_args(msg, NULL,
                                  DBUS_TYPE_BOOLEAN, &closed,
                                  DBUS_TYPE_INVALID) && closed) {
            fprintf(stderr, "isde-session: lid closed, action=%d\n",
                    (int)s->lid_action);
            switch (s->lid_action) {
            case LID_ACTION_SUSPEND:
                dm_dbus_call("Suspend");
                break;
            case LID_ACTION_HIBERNATE:
                dm_dbus_call("Suspend");
                break;
            case LID_ACTION_LOCK:
                dm_dbus_call("Lock");
                break;
            case LID_ACTION_NOTHING:
                break;
            }
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void init_system_bus(Session *s)
{
    DBusError err;
    dbus_error_init(&err);
    s->system_bus = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!s->system_bus) {
        fprintf(stderr, "isde-session: system bus: %s\n",
                err.message ? err.message : "cannot connect");
        dbus_error_free(&err);
        return;
    }

    dbus_connection_set_exit_on_disconnect(s->system_bus, FALSE);

    /* Subscribe to ConfirmationRequested signals from the DM */
    dbus_bus_add_match(s->system_bus,
        "type='signal',"
        "interface='org.isde.DisplayManager',"
        "member='ConfirmationRequested'",
        &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "isde-session: D-Bus match: %s\n", err.message);
        dbus_error_free(&err);
    }

    /* Subscribe to LidSwitchChanged signals from the DM */
    dbus_bus_add_match(s->system_bus,
        "type='signal',"
        "interface='org.isde.DisplayManager',"
        "member='LidSwitchChanged'",
        &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "isde-session: D-Bus match (lid): %s\n", err.message);
        dbus_error_free(&err);
    }

    dbus_connection_add_filter(s->system_bus, system_bus_filter, s, NULL);
    dbus_error_free(&err);
}

/* ---------- fd-ready dispatchers (driven by poll()) ---------- */

static void dbus_input_ready(Session *s)
{
    if (s->dbus) {
        isde_dbus_dispatch(s->dbus);
    }
}

static void system_bus_input_ready(Session *s)
{
    if (s->system_bus) {
        dbus_connection_read_write(s->system_bus, 0);
        while (dbus_connection_dispatch(s->system_bus) ==
               DBUS_DISPATCH_DATA_REMAINS) {
            /* drain */
        }
    }
}

static void session_bus_input_ready(Session *s)
{
    if (s->session_bus) {
        dbus_connection_read_write(s->session_bus, 0);
        while (dbus_connection_dispatch(s->session_bus) ==
               DBUS_DISPATCH_DATA_REMAINS) {
            /* drain */
        }
    }
}

/* ---------- Periodic check timer ---------- */

static void check_timer_fire(Session *s)
{
    xcb_connection_t *conn = (xcb_connection_t *)s->server_context;
    s->check_deadline_ms = now_ms() + 500;  /* re-arm */

    if (s->reload_display) {
        s->reload_display = 0;
        apply_appearance_settings();
        fprintf(stderr, "isde-session: display config changed, "
                "restarting WM and panel\n");
        restart_ui_children(s);
    }

    if (s->reload_appearance) {
        s->reload_appearance = 0;
        isde_theme_reload();
        if (conn) {
            isde_theme_set_resource_manager(conn, session_root(s));
        }
        fprintf(stderr, "isde-session: appearance changed\n");
    }

    if (s->reload_power) {
        s->reload_power = 0;
        apply_power_settings(s);
    }
}

/* ---------- public API ---------- */

int session_init(Session *s)
{
    xcb_connection_t *conn = (xcb_connection_t *)s->server_context;
    memset(s, 0, sizeof(*s));
    s->death_pipe[0] = s->death_pipe[1] = -1;
    s->sigchld_pipe[0] = s->sigchld_pipe[1] = -1;

    /* Load isde.toml from XDG config dirs */
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *sess = isde_config_table(root, "session");
        if (sess) {
            const char *wm = isde_config_string(sess, "window_manager", NULL);
            if (wm) { s->wm_command = strdup(wm); }

            const char *panel = isde_config_string(sess, "panel", NULL);
            if (panel) { s->panel_command = strdup(panel); }

            const char *fm = isde_config_string(sess, "file_manager", NULL);
            if (fm) { s->fm_command = strdup(fm); }

            s->idle_lock_sec = (int)isde_config_int(sess, "lock_timeout", 0);
        }
        isde_config_free(cfg);
    } else {
        fprintf(stderr, "isde-session: %s (using defaults)\n", errbuf);
    }

    /* Defaults if not configured */
    if (!s->wm_command) {
        s->wm_command = strdup("isde-wm");
    }
    if (!s->panel_command) {
        s->panel_command = strdup("isde-panel");
    }

    /* Load autostart file */
    char *autostart_path = isde_xdg_find_config("autostart");
    if (autostart_path) {
        autostart_load(s, autostart_path);
        free(autostart_path);
    }

    /* Also load XDG autostart .desktop files */
    autostart_load_xdg(s);

    /* Apply settings from config before starting components */
    apply_appearance_settings();
    apply_input_settings();
    apply_power_settings(s);

    /* Self-pipe to deliver SIGCHLD into the poll() loop. Non-blocking so the
     * handler never stalls and the drain never blocks. */
    if (pipe(s->sigchld_pipe) < 0) {
        perror("isde-session: sigchld pipe");
        s->sigchld_pipe[0] = s->sigchld_pipe[1] = -1;
    } else {
        for (int i = 0; i < 2; i++) {
            int fl = fcntl(s->sigchld_pipe[i], F_GETFL, 0);
            fcntl(s->sigchld_pipe[i], F_SETFL, fl | O_NONBLOCK);
            fl = fcntl(s->sigchld_pipe[i], F_GETFD, 0);
            fcntl(s->sigchld_pipe[i], F_SETFD, fl | FD_CLOEXEC);
        }
        sigchld_write_fd = s->sigchld_pipe[1];
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    /* D-Bus session bus for settings change notifications */
    s->dbus = isde_dbus_init();
    if (s->dbus) {
        isde_dbus_settings_subscribe(s->dbus, on_settings_changed, s);
    }

    /* D-Bus system bus for DM ConfirmationRequested signal */
    init_system_bus(s);

    /* org.freedesktop.ScreenSaver inhibit service on session bus */
    init_screensaver_service(s);

    /* org.isde.Session command service on the same session bus */
    init_session_command_service(s);

    /* XCB connection for theme publishing, DPMS, and screensaver query */
    s->server_context = (void *)xcb_connect(getenv("DISPLAY"), &s->screen_num);
    if (!xcb_connection_has_error(conn)) {
        /* Publish theme to RESOURCE_MANAGER so all X clients inherit it */
        isde_theme_set_resource_manager(conn, session_root(s));

        xcb_flush(s->server_context);
    } else {
        xcb_disconnect(s->server_context);
        s->server_context = NULL;
    }

    /* Liveness pipe — children inherit the read end; EOF signals parent death */
    if (pipe(s->death_pipe) < 0) {
        perror("isde-session: pipe");
        s->death_pipe[0] = s->death_pipe[1] = -1;
    }

    s->running = 1;
    return 0;
}

void child_kill_all(Session *s)
{
    xcb_connection_t *conn = (xcb_connection_t *)s->server_context;
    struct timespec ts = { 0, 50000000 }; /* 50ms */

    /* Disable respawning */
    for (Child *c = s->children; c; c = c->next) {
        c->respawn = 0;
    }

    /* Phase 0: ask the WM to close every managed client via _NET_CLOSE_WINDOW.
     * Daemonized apps (VSCodium, Electron apps that call setsid) escape our
     * process group, so kill(0, SIGTERM) alone misses them. Closing them by
     * their top-level window goes through the WM regardless of PID lineage
     * and also gives apps a chance to save state before exiting. */
    if (conn && !xcb_connection_has_error(conn)) {
        IsdeEwmh *ewmh = isde_ewmh_init(conn, s->screen_num);
        if (ewmh) {
            xcb_window_t *wins = NULL;
            int n = isde_ewmh_get_client_list(ewmh, &wins);
            for (int i = 0; i < n; i++) {
                isde_ewmh_request_close_window(ewmh, wins[i]);
            }
            free(wins);
            xcb_flush(conn);
            isde_ewmh_free(ewmh);

            /* Give apps up to ~3 seconds to save state and exit cleanly. */
            for (int i = 0; i < 60; i++) {
                int status;
                pid_t pid;
                while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                    Child *c = child_find_pid(s, pid);
                    if (c) { child_remove(s, c); }
                }
                nanosleep(&ts, NULL);
            }
        }
    }

    /* Phase 1: SIGTERM everything in our process group */
    kill(0, SIGTERM);

    /* Phase 2: Poll for up to 2 seconds, reaping as children exit */
    for (int i = 0; i < 40 && s->children; i++) {
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            Child *c = child_find_pid(s, pid);
            if (c) { child_remove(s, c); }
        }
        if (!s->children) { break; }
        nanosleep(&ts, NULL);
    }

    /* Phase 3: SIGKILL everything in our process group, including us.
     * Our job is done — the parent (display manager/init) reaps us. */
    if (s->children) {
        kill(0, SIGKILL);
        /* unreachable */
    }

    /* All children exited cleanly — free any remaining list entries */
    while (s->children) {
        Child *c = s->children;
        s->children = c->next;
        free(c->command);
        free(c);
    }
}

void session_run(Session *s)
{
    fprintf(stderr, "isde-session: starting WM: %s\n", s->wm_command);
    Child *wm = child_spawn(s, s->wm_command, 1, 1);
    if (!wm) {
        fprintf(stderr, "isde-session: failed to start WM\n");
        return;
    }
    /* Give the WM a moment to set SubstructureRedirect */
    usleep(500000);

    if (s->panel_command) {
        fprintf(stderr, "isde-session: starting panel: %s\n",
                s->panel_command);
        Child *panel = child_spawn(s, s->panel_command, 1, 0);
        if (panel) { panel->is_panel = 1; }
    }

    for (int i = 0; i < s->autostart_count; i++) {
        fprintf(stderr, "isde-session: autostart: %s\n",
                s->autostart_cmds[i]);
        child_spawn(s, s->autostart_cmds[i], s->autostart_respawn[i], 0);
    }

    /* Collect fd-based input sources for poll() */
    int dbus_fd = s->dbus ? isde_dbus_get_fd(s->dbus) : -1;

    int sys_fd = -1;
    if (s->system_bus) {
        dbus_connection_get_unix_fd(s->system_bus, &sys_fd);
    }

    int sess_fd = -1;
    if (s->session_bus) {
        dbus_connection_get_unix_fd(s->session_bus, &sess_fd);
    }

    /* Periodic timer for WM-alive check and appearance reload */
    s->check_deadline_ms = now_ms() + 500;

    /* Idle timer (polls every 10s) — lock and/or suspend */
    if (s->idle_lock_sec > 0 || s->idle_suspend_sec > 0) {
        s->idle_deadline_ms = now_ms() + 10000;
    }

    /* poll() event loop. Indices: 0 sigchld, then the optional fd sources. */
    while (s->running) {
        struct pollfd pfd[4];
        int slot[4];   /* maps pfd index -> source kind */
        enum { SRC_SIGCHLD, SRC_DBUS, SRC_SYSBUS, SRC_SESSBUS };
        int n = 0;

        if (s->sigchld_pipe[0] >= 0) {
            pfd[n].fd = s->sigchld_pipe[0]; pfd[n].events = POLLIN;
            slot[n] = SRC_SIGCHLD; n++;
        }
        if (dbus_fd >= 0) {
            pfd[n].fd = dbus_fd; pfd[n].events = POLLIN;
            slot[n] = SRC_DBUS; n++;
        }
        if (sys_fd >= 0) {
            pfd[n].fd = sys_fd; pfd[n].events = POLLIN;
            slot[n] = SRC_SYSBUS; n++;
        }
        if (sess_fd >= 0) {
            pfd[n].fd = sess_fd; pfd[n].events = POLLIN;
            slot[n] = SRC_SESSBUS; n++;
        }

        /* Compute the poll timeout from the soonest armed timer. libdbus may
         * hold buffered messages that won't re-trigger fd readability, so poll
         * with no wait when either bus still has data to dispatch. */
        long long now = now_ms();
        long long deadline = -1;
        if (s->check_deadline_ms > 0) { deadline = s->check_deadline_ms; }
        if (s->idle_deadline_ms > 0 &&
            (deadline < 0 || s->idle_deadline_ms < deadline)) {
            deadline = s->idle_deadline_ms;
        }
        int timeout = -1;
        if (deadline >= 0) {
            timeout = (int)(deadline - now);
            if (timeout < 0) { timeout = 0; }
        }
        if ((s->system_bus && dbus_connection_get_dispatch_status(s->system_bus)
                == DBUS_DISPATCH_DATA_REMAINS) ||
            (s->session_bus && dbus_connection_get_dispatch_status(s->session_bus)
                == DBUS_DISPATCH_DATA_REMAINS)) {
            timeout = 0;
        }

        int r = poll(pfd, n, timeout);
        if (r < 0) {
            if (errno == EINTR) { continue; }
            perror("isde-session: poll");
            break;
        }

        for (int i = 0; i < n && r > 0; i++) {
            if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR))) { continue; }
            switch (slot[i]) {
            case SRC_SIGCHLD: {
                char buf[64];
                while (read(s->sigchld_pipe[0], buf, sizeof(buf)) > 0) {
                    /* drain coalesced wakeups */
                }
                sigchld_dispatch(s);
                break;
            }
            case SRC_DBUS:    dbus_input_ready(s); break;
            /* The system/session buses are serviced unconditionally below so
             * that libdbus-buffered messages are dispatched even when no new
             * fd activity arrived; their poll wakeups need no separate case. */
            case SRC_SYSBUS:
            case SRC_SESSBUS:
                break;
            }
        }

        /* Service the D-Bus buses (drains the read fd and dispatches any
         * messages libdbus already buffered in-process). */
        system_bus_input_ready(s);
        session_bus_input_ready(s);

        /* Fire any expired timers. */
        now = now_ms();
        if (s->check_deadline_ms > 0 && now >= s->check_deadline_ms) {
            check_timer_fire(s);
        }
        if (s->idle_deadline_ms > 0 && now >= s->idle_deadline_ms) {
            idle_timer_fire(s);
        }
    }
}

void session_shutdown(Session *s)
{
    s->running = 0;
    child_kill_all(s);
}

void session_cleanup(Session *s)
{
    xcb_connection_t *conn = (xcb_connection_t *)s->server_context;
    if (s->confirm_pid > 0) {
        kill(s->confirm_pid, SIGTERM);
        waitpid(s->confirm_pid, NULL, 0);
        s->confirm_pid = 0;
    }
    child_kill_all(s);
    if (s->death_pipe[0] >= 0) { close(s->death_pipe[0]); }
    if (s->death_pipe[1] >= 0) { close(s->death_pipe[1]); }
    sigchld_write_fd = -1;
    if (s->sigchld_pipe[0] >= 0) { close(s->sigchld_pipe[0]); }
    if (s->sigchld_pipe[1] >= 0) { close(s->sigchld_pipe[1]); }
    autostart_free(s);
    isde_dbus_free(s->dbus);
    if (conn) { xcb_disconnect(conn); }
    if (s->system_bus) {
        dbus_connection_remove_filter(s->system_bus, system_bus_filter, s);
        dbus_connection_unref(s->system_bus);
    }
    if (s->session_bus) {
        dbus_connection_remove_filter(s->session_bus, session_bus_filter, s);
        dbus_connection_unref(s->session_bus);
    }
    free(s->wm_command);
    free(s->panel_command);
    free(s->fm_command);
}
