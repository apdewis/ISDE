#define _POSIX_C_SOURCE 200809L
/*
 * session.c — session initialization, config loading, startup sequence, main loop
 */
#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <xcb/xcb.h>
#include <xcb/xkb.h>
#include <dbus/dbus.h>

#include "isde/isde-theme.h"

#include <ISW/Shell.h>
#include <ISW/StringDefs.h>
#include <ISW/IntrinsicP.h>

/* ---------- DM D-Bus helper ---------- */

static void dm_dbus_call(const char *method)
{
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!conn) {
        fprintf(stderr, "isde-session: D-Bus: %s\n",
                err.message ? err.message : "cannot connect");
        dbus_error_free(&err);
        return;
    }

    DBusMessage *msg = dbus_message_new_method_call(
        "org.isde.DisplayManager",
        "/org/isde/DisplayManager",
        "org.isde.DisplayManager",
        method);
    if (msg) {
        dbus_connection_send(conn, msg, NULL);
        dbus_connection_flush(conn);
        dbus_message_unref(msg);
    }
    dbus_connection_unref(conn);
    dbus_error_free(&err);
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

/* ---------- D-Bus settings callback ---------- */

static void on_settings_changed(const char *section, const char *key,
                                void *user_data)
{
    (void)key;
    Session *s = (Session *)user_data;
    if (strcmp(section, "appearance") == 0 ||
        strcmp(section, "display") == 0 ||
        strcmp(section, "*") == 0) {
        s->reload_appearance = 1;
    }
}

static void restart_ui_children(Session *s)
{
    /* SIGTERM the WM and panel — child_reap will respawn them */
    for (Child *c = s->children; c; c = c->next) {
        if (c->is_wm || c->is_panel) {
            kill(c->pid, SIGTERM);
        }
    }
}

/* ---------- SIGCHLD ---------- */

static IswAppContext sigchld_app;
static IswSignalId   sigchld_xt_id;

static void sigchld_handler(int sig)
{
    (void)sig;
    if (sigchld_app) {
        IswNoticeSignal(sigchld_xt_id);
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

static void sigchld_xt_cb(IswPointer client_data, IswSignalId *id)
{
    (void)id;
    Session *s = (Session *)client_data;
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

    dbus_connection_add_filter(s->system_bus, system_bus_filter, s, NULL);
    dbus_error_free(&err);
}

/* ---------- Xt input callbacks ---------- */

static void dbus_input_cb(IswPointer client_data, int *fd, IswInputId *id)
{
    (void)fd; (void)id;
    Session *s = (Session *)client_data;
    if (s->dbus) {
        isde_dbus_dispatch(s->dbus);
    }
}

static void system_bus_input_cb(IswPointer client_data, int *fd, IswInputId *id)
{
    (void)fd; (void)id;
    Session *s = (Session *)client_data;
    if (s->system_bus) {
        dbus_connection_read_write(s->system_bus, 0);
        while (dbus_connection_dispatch(s->system_bus) ==
               DBUS_DISPATCH_DATA_REMAINS) {
            /* drain */
        }
    }
}

static void xcb_input_cb(IswPointer client_data, int *fd, IswInputId *id)
{
    (void)fd; (void)id;
    Session *s = (Session *)client_data;

    xcb_generic_event_t *ev;
    while ((ev = xcb_poll_for_event(s->conn)) != NULL) {
        uint32_t cmd, d0, d1, d2, d3;
        if (isde_ipc_decode(s->ipc, ev, &cmd, &d0, &d1, &d2, &d3)) {
            if (cmd == ISDE_CMD_LOGOUT) {
                fprintf(stderr, "isde-session: logout requested\n");
                show_confirmation(s, "logout");
            } else if (cmd == ISDE_CMD_LOCK) {
                fprintf(stderr, "isde-session: lock requested\n");
                dm_dbus_call("Lock");
            } else if (cmd == ISDE_CMD_SHUTDOWN) {
                fprintf(stderr, "isde-session: shutdown requested\n");
                show_confirmation(s, "shutdown");
            } else if (cmd == ISDE_CMD_REBOOT) {
                fprintf(stderr, "isde-session: reboot requested\n");
                show_confirmation(s, "reboot");
            } else if (cmd == ISDE_CMD_SUSPEND) {
                fprintf(stderr, "isde-session: suspend requested\n");
                show_confirmation(s, "suspend");
            }
        }
        free(ev);
    }
}

/* ---------- Periodic check timer ---------- */

static void check_timer_cb(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    Session *s = (Session *)client_data;

    if (s->reload_appearance) {
        s->reload_appearance = 0;
        fprintf(stderr, "isde-session: appearance changed, "
                "restarting WM and panel\n");
        restart_ui_children(s);
    }

    /* Re-arm the timer */
    s->check_timer = IswAppAddTimeOut(s->app, 500, check_timer_cb, s);
}

/* ---------- public API ---------- */

int session_init(Session *s)
{
    memset(s, 0, sizeof(*s));

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

    /* Initialize Xt for confirmation dialogs */
    char **fallbacks = isde_theme_build_resources();
    int argc = 1;
    char *argv[] = { "isde-session", NULL };
    s->toplevel = IswAppInitialize(&s->app, "ISDE-Session",
                                  NULL, 0, &argc, argv,
                                  fallbacks, NULL, 0);

    /* Realize but don't map — needed as parent for popup shells */
    Arg tl_args[20];
    Cardinal tl_n = 0;
    IswSetArg(tl_args[tl_n], IswNmappedWhenManaged, False); tl_n++;
    IswSetArg(tl_args[tl_n], IswNwidth, 1);                 tl_n++;
    IswSetArg(tl_args[tl_n], IswNheight, 1);                tl_n++;
    IswSetValues(s->toplevel, tl_args, tl_n);
    IswRealizeWidget(s->toplevel);

    /* Install SIGCHLD handler via Xt signal mechanism */
    sigchld_app = s->app;
    sigchld_xt_id = IswAppAddSignal(s->app, sigchld_xt_cb, s);
    s->sigchld_id = sigchld_xt_id;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    /* D-Bus session bus for settings change notifications */
    s->dbus = isde_dbus_init();
    if (s->dbus) {
        isde_dbus_settings_subscribe(s->dbus, on_settings_changed, s);
    }

    /* D-Bus system bus for DM ConfirmationRequested signal */
    init_system_bus(s);

    /* XCB connection for IPC (ISDE_CMD_LOGOUT etc.) */
    int screen_num;
    s->conn = xcb_connect(getenv("DISPLAY"), &screen_num);
    if (!xcb_connection_has_error(s->conn)) {
        s->ipc = isde_ipc_init(s->conn, screen_num);

        /* Select StructureNotify on root so IPC ClientMessages are delivered */
        xcb_screen_iterator_t it =
            xcb_setup_roots_iterator(xcb_get_setup(s->conn));
        for (int i = 0; i < screen_num; i++) { xcb_screen_next(&it); }
        uint32_t ev_mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
        xcb_change_window_attributes(s->conn, it.data->root,
                                     XCB_CW_EVENT_MASK, &ev_mask);
        xcb_flush(s->conn);
    } else {
        xcb_disconnect(s->conn);
        s->conn = NULL;
    }

    s->running = 1;
    return 0;
}

void session_run(Session *s)
{
    /* Phase 1: Start window manager and wait briefly for it to claim root */
    fprintf(stderr, "isde-session: starting WM: %s\n", s->wm_command);
    Child *wm = child_spawn(s, s->wm_command, 1, 1);
    if (!wm) {
        fprintf(stderr, "isde-session: failed to start WM\n");
        return;
    }
    /* Give the WM a moment to set SubstructureRedirect */
    usleep(500000);

    /* Phase 2: Start panel if configured */
    if (s->panel_command) {
        fprintf(stderr, "isde-session: starting panel: %s\n",
                s->panel_command);
        Child *panel = child_spawn(s, s->panel_command, 1, 0);
        if (panel) { panel->is_panel = 1; }
    }

    /* Phase 3: Start file manager if configured */
    if (s->fm_command) {
        fprintf(stderr, "isde-session: starting file manager: %s\n",
                s->fm_command);
        child_spawn(s, s->fm_command, 1, 0);
    }

    /* Phase 4: Start autostart entries */
    for (int i = 0; i < s->autostart_count; i++) {
        fprintf(stderr, "isde-session: autostart: %s\n",
                s->autostart_cmds[i]);
        child_spawn(s, s->autostart_cmds[i], s->autostart_respawn[i], 0);
    }

    /* Register fd-based input sources with Xt */
    int dbus_fd = s->dbus ? isde_dbus_get_fd(s->dbus) : -1;
    int xcb_fd  = s->conn ? xcb_get_file_descriptor(s->conn) : -1;

    if (dbus_fd >= 0) {
        IswAppAddInput(s->app, dbus_fd, (IswPointer)IswInputReadMask,
                      dbus_input_cb, s);
    }

    int sys_fd = -1;
    if (s->system_bus) {
        dbus_connection_get_unix_fd(s->system_bus, &sys_fd);
        if (sys_fd >= 0) {
            IswAppAddInput(s->app, sys_fd, (IswPointer)IswInputReadMask,
                          system_bus_input_cb, s);
        }
    }

    if (xcb_fd >= 0) {
        IswAppAddInput(s->app, xcb_fd, (IswPointer)IswInputReadMask,
                      xcb_input_cb, s);
    }

    /* Periodic timer for WM-alive check and appearance reload */
    s->check_timer = IswAppAddTimeOut(s->app, 500, check_timer_cb, s);

    /* Xt event loop */
    while (s->running) {
        IswAppProcessEvent(s->app, IswIMAll);

        if (IswAppGetExitFlag(s->app)) {
            break;
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
    if (s->confirm_pid > 0) {
        kill(s->confirm_pid, SIGTERM);
        waitpid(s->confirm_pid, NULL, 0);
        s->confirm_pid = 0;
    }
    child_kill_all(s);
    autostart_free(s);
    isde_dbus_free(s->dbus);
    isde_ipc_free(s->ipc);
    if (s->conn) { xcb_disconnect(s->conn); }
    if (s->system_bus) {
        dbus_connection_remove_filter(s->system_bus, system_bus_filter, s);
        dbus_connection_unref(s->system_bus);
    }
    if (s->toplevel) {
        IswDestroyWidget(s->toplevel);
    }
    free(s->wm_command);
    free(s->panel_command);
    free(s->fm_command);
}
