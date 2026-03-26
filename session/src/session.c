#define _POSIX_C_SOURCE 200809L
/*
 * session.c — session initialization, config loading, startup sequence, main loop
 */
#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include <xcb/xcb.h>
#include <xcb/xkb.h>

/* ---------- apply input settings from config ---------- */

static void apply_appearance_settings(void)
{
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (!cfg) return;

    IsdeConfigTable *root = isde_config_root(cfg);
    IsdeConfigTable *appear = isde_config_table(root, "appearance");
    if (appear) {
        const char *cursor = isde_config_string(appear, "cursor_theme", NULL);
        if (cursor) {
            setenv("XCURSOR_THEME", cursor, 1);
            fprintf(stderr, "isde-session: cursor theme=%s\n", cursor);
        }
        const char *cursor_size = isde_config_string(appear, "cursor_size", NULL);
        if (cursor_size)
            setenv("XCURSOR_SIZE", cursor_size, 1);
    }
    isde_config_free(cfg);
}

static void apply_input_settings(void)
{
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (!cfg) return;

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

        xcb_xkb_set_controls(conn,
            XCB_XKB_ID_USE_CORE_KBD,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            rep_delay > 0 ? rep_delay : 300,
            rep_int > 0 ? rep_int : 30,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            XCB_XKB_BOOL_CTRL_REPEAT_KEYS,
            0,
            NULL);
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
    if (strcmp(section, "appearance") == 0 || strcmp(section, "*") == 0)
        s->reload_appearance = 1;
}

static void restart_ui_children(Session *s)
{
    /* SIGTERM the WM and panel — child_reap will respawn them */
    for (Child *c = s->children; c; c = c->next) {
        if (c->is_wm || c->is_panel)
            kill(c->pid, SIGTERM);
    }
}

/* SIGCHLD handler — just sets a flag so the main loop can reap */
static volatile sig_atomic_t got_sigchld = 0;

static void sigchld_handler(int sig)
{
    (void)sig;
    got_sigchld = 1;
}

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
            if (wm) s->wm_command = strdup(wm);

            const char *panel = isde_config_string(sess, "panel", NULL);
            if (panel) s->panel_command = strdup(panel);

            const char *fm = isde_config_string(sess, "file_manager", NULL);
            if (fm) s->fm_command = strdup(fm);
        }
        isde_config_free(cfg);
    } else {
        fprintf(stderr, "isde-session: %s (using defaults)\n", errbuf);
    }

    /* Defaults if not configured */
    if (!s->wm_command)
        s->wm_command = strdup("isde-wm");

    /* Load autostart file */
    char *autostart_path = isde_xdg_find_config("autostart");
    if (autostart_path) {
        autostart_load(s, autostart_path);
        free(autostart_path);
    }

    /* Also load XDG autostart .desktop files */
    autostart_load_xdg(s);

    /* Install SIGCHLD handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    /* Apply settings from config before starting components */
    apply_appearance_settings();
    apply_input_settings();

    /* D-Bus for settings change notifications */
    s->dbus = isde_dbus_init();
    if (s->dbus)
        isde_dbus_settings_subscribe(s->dbus, on_settings_changed, s);

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
        if (panel) panel->is_panel = 1;
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

    /* Main loop — poll D-Bus fd and handle signals */
    int dbus_fd = s->dbus ? isde_dbus_get_fd(s->dbus) : -1;

    while (s->running) {
        if (got_sigchld) {
            got_sigchld = 0;
            child_reap(s);
        }

        if (s->reload_appearance) {
            s->reload_appearance = 0;
            fprintf(stderr, "isde-session: appearance changed, "
                    "restarting WM and panel\n");
            restart_ui_children(s);
        }

        /* If the WM died and wasn't respawned, shut down the session */
        int wm_alive = 0;
        for (Child *c = s->children; c; c = c->next) {
            if (c->is_wm) { wm_alive = 1; break; }
        }
        if (!wm_alive) {
            fprintf(stderr, "isde-session: WM exited, ending session\n");
            s->running = 0;
            break;
        }

        /* Poll D-Bus fd (or just sleep if no D-Bus) */
        if (dbus_fd >= 0) {
            struct pollfd pfd = { .fd = dbus_fd, .events = POLLIN };
            poll(&pfd, 1, 100);
            if (pfd.revents & POLLIN)
                isde_dbus_dispatch(s->dbus);
        } else {
            pause();
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
    child_kill_all(s);
    autostart_free(s);
    isde_dbus_free(s->dbus);
    free(s->wm_command);
    free(s->panel_command);
    free(s->fm_command);
}
