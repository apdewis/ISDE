#define _POSIX_C_SOURCE 200809L
#include "term.h"

#include <ISW/Intrinsic.h>
#include <ISW/Shell.h>
#include <ISW/IswArgMacros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

#include "isde/isde-theme.h"

typedef struct {
    IswAppContext  app;
    Widget         toplevel;
    TermWidget    *term;
    TermPty       *pty;
    IsdeDBus      *dbus;
    int            running;
} TermApp;

static TermApp g_app;

static void pty_read_cb(const char *buf, size_t n, void *user)
{
    TermApp *a = (TermApp *)user;
    term_widget_feed(a->term, buf, n);
}

static void pty_exit_cb(int status, void *user)
{
    (void)status;
    TermApp *a = (TermApp *)user;
    a->running = 0;
    /* Leave the window open if exited quickly? For now just quit. */
    IswAppSetExitFlag(a->app);
}

static void dbus_input_cb(IswPointer closure, int *source, IswInputId *id)
{
    (void)source; (void)id;
    isde_dbus_dispatch((IsdeDBus *)closure);
}

static void on_settings_changed(const char *section, const char *key, void *user)
{
    (void)key;
    TermApp *a = (TermApp *)user;
    if (!section) return;
    if (strcmp(section, "terminal") == 0 ||
        strcmp(section, "appearance") == 0 ||
        strcmp(section, "fonts") == 0) {
        isde_theme_reload();
        TermConfig cfg;
        term_config_load(&cfg);
        term_widget_apply_config(a->term, &cfg);
    }
}

static void usage(void)
{
    fprintf(stderr,
        "Usage: isde-term [--title TITLE] [--geometry COLSxROWS]\n"
        "                 [--working-directory DIR] [-e CMD [ARGS...]]\n");
}

int main(int argc, char **argv)
{
    const char *title = "Terminal";
    int cols = 80, rows = 24;
    const char *workdir = NULL;
    char **exec_argv = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--title") == 0 && i + 1 < argc) {
            title = argv[++i];
        } else if (strcmp(a, "--geometry") == 0 && i + 1 < argc) {
            int c, r;
            if (sscanf(argv[++i], "%dx%d", &c, &r) == 2 && c > 0 && r > 0) {
                cols = c; rows = r;
            }
        } else if ((strcmp(a, "--working-directory") == 0 ||
                    strcmp(a, "--workdir") == 0) && i + 1 < argc) {
            workdir = argv[++i];
        } else if (strcmp(a, "-e") == 0) {
            if (i + 1 < argc) {
                exec_argv = &argv[i + 1];
            }
            break;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage();
            return 0;
        }
    }

    if (workdir) {
        if (chdir(workdir) != 0) {
            fprintf(stderr, "isde-term: chdir %s: failed\n", workdir);
        }
    }

    memset(&g_app, 0, sizeof(g_app));
    g_app.running = 1;

    g_app.toplevel = IswAppInitialize(&g_app.app, "ISDE-Term",
                                      NULL, 0, &argc, argv,
                                      NULL, NULL, 0);
    isde_theme_merge_xrm(g_app.toplevel);

    TermConfig cfg;
    term_config_load(&cfg);

    g_app.term = term_widget_create(g_app.toplevel, "terminal", &cfg, cols, rows);
    if (!g_app.term) {
        fprintf(stderr, "isde-term: failed to create widget\n");
        return 1;
    }

    int px_w, px_h;
    term_widget_preferred_pixels(g_app.term, cols, rows, &px_w, &px_h);
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgWidth(&ab, px_w);
    IswArgHeight(&ab, px_h);
    IswArgTitle(&ab, (char *)title);
    IswSetValues(g_app.toplevel, ab.args, ab.count);

    IswRealizeWidget(g_app.toplevel);

    /* Set WM_CLASS so the panel/taskbar can group windows and resolve the
     * .desktop entry (which matches by exec basename = "isde-term"). */
    {
        xcb_connection_t *c = IswDisplay(g_app.toplevel);
        xcb_window_t win = IswWindow(g_app.toplevel);
        static const char cls[] = "isde-term\0isde-term";
        xcb_icccm_set_wm_class(c, win, sizeof(cls) - 1, cls);
        xcb_flush(c);
    }

    /* Launch shell */
    const char *shell = getenv("SHELL");
    if (!shell || !shell[0]) shell = "/bin/sh";
    char *default_argv[2] = { (char *)shell, NULL };
    char **use_argv = exec_argv ? exec_argv : default_argv;

    g_app.pty = term_pty_spawn(g_app.app, shell, use_argv,
                               cols, rows,
                               pty_read_cb, pty_exit_cb, &g_app);
    if (!g_app.pty) {
        fprintf(stderr, "isde-term: failed to spawn shell\n");
        return 1;
    }
    term_widget_attach_pty(g_app.term, g_app.pty);

    /* DBus settings subscription */
    g_app.dbus = isde_dbus_init();
    if (g_app.dbus) {
        int fd = isde_dbus_get_fd(g_app.dbus);
        if (fd >= 0) {
            IswAppAddInput(g_app.app, fd,
                          (IswPointer)(intptr_t)IswInputReadMask,
                          dbus_input_cb, g_app.dbus);
        }
        isde_dbus_settings_subscribe(g_app.dbus, on_settings_changed, &g_app);
    }

    IswAppMainLoop(g_app.app);

    term_pty_close(g_app.pty);
    term_widget_destroy(g_app.term);
    if (g_app.dbus) isde_dbus_free(g_app.dbus);
    return 0;
}
