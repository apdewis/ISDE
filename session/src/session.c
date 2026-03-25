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
#include <sys/wait.h>

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
        child_spawn(s, s->panel_command, 1, 0);
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

    /* Main loop — wait for signals and reap children */
    while (s->running) {
        if (got_sigchld) {
            got_sigchld = 0;
            child_reap(s);
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

        /* Sleep until next signal */
        pause();
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
    free(s->wm_command);
    free(s->panel_command);
    free(s->fm_command);
}
