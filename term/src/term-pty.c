#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include "term.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

struct TermPty {
    IswAppContext app;
    pid_t         pid;
    int           fd;
    IswInputId    input_id;
    IswSignalId   sigchld_id;
    TermPtyReadCb on_read;
    TermPtyExitCb on_exit;
    void         *user;
    bool          closed;
};

static TermPty *g_pty_for_signal;  /* single-terminal-per-process assumption */

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) {
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    fl = fcntl(fd, F_GETFD, 0);
    if (fl >= 0) {
        fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
    }
}

static void pty_input_cb(IswPointer closure, int *source, IswInputId *id)
{
    (void)source; (void)id;
    TermPty *p = (TermPty *)closure;
    char buf[4096];
    for (;;) {
        ssize_t n = read(p->fd, buf, sizeof(buf));
        if (n > 0) {
            if (p->on_read) p->on_read(buf, (size_t)n, p->user);
            if ((size_t)n < sizeof(buf)) break;
            continue;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        }
        /* EOF or fatal error -> child probably exited */
        if (!p->closed) {
            p->closed = true;
            if (p->input_id) { IswRemoveInput(p->input_id); p->input_id = 0; }
            int status = 0;
            if (p->pid > 0) waitpid(p->pid, &status, WNOHANG);
            if (p->on_exit) p->on_exit(status, p->user);
        }
        return;
    }
}

static void sigchld_handler(int sig)
{
    (void)sig;
    if (g_pty_for_signal) {
        IswNoticeSignal(g_pty_for_signal->sigchld_id);
    }
}

static void sigchld_signal_cb(IswPointer closure, IswSignalId *id)
{
    (void)id;
    TermPty *p = (TermPty *)closure;
    int status = 0;
    pid_t r = waitpid(p->pid, &status, WNOHANG);
    if (r == p->pid && !p->closed) {
        p->closed = true;
        if (p->input_id) { IswRemoveInput(p->input_id); p->input_id = 0; }
        if (p->on_exit) p->on_exit(status, p->user);
    }
}

TermPty *term_pty_spawn(IswAppContext app,
                        const char *shell, char *const *argv,
                        unsigned cols, unsigned rows,
                        TermPtyReadCb on_read,
                        TermPtyExitCb on_exit,
                        void *user)
{
    if (!shell || !shell[0]) shell = getenv("SHELL");
    if (!shell || !shell[0]) shell = "/bin/sh";

    struct winsize ws = { .ws_row = rows ? rows : 24,
                          .ws_col = cols ? cols : 80 };

    int master = -1;
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) {
        perror("forkpty");
        return NULL;
    }

    if (pid == 0) {
        /* child */
        setenv("TERM", "xterm-256color", 1);
        unsetenv("COLUMNS");
        unsetenv("LINES");

        {
            const char *lc_all   = getenv("LC_ALL");
            const char *lc_ctype = getenv("LC_CTYPE");
            const char *lang     = getenv("LANG");
            int has_utf8 = 0;
            const char *probes[] = { lc_all, lc_ctype, lang };
            for (size_t i = 0; i < sizeof(probes)/sizeof(probes[0]); i++) {
                const char *v = probes[i];
                if (!v || !*v) continue;
                if (strstr(v, "UTF-8") || strstr(v, "utf8") ||
                    strstr(v, "UTF8")  || strstr(v, "utf-8")) {
                    has_utf8 = 1;
                    break;
                }
            }
            if (!has_utf8) setenv("LANG", "C.UTF-8", 1);
        }

        char *fallback_argv[2];
        if (!argv || !argv[0]) {
            fallback_argv[0] = (char *)shell;
            fallback_argv[1] = NULL;
            argv = fallback_argv;
        }
        execvp(argv[0], argv);
        fprintf(stderr, "isde-term: exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    set_nonblock(master);

    TermPty *p = calloc(1, sizeof(*p));
    p->app = app;
    p->pid = pid;
    p->fd  = master;
    p->on_read = on_read;
    p->on_exit = on_exit;
    p->user = user;

    p->input_id = IswAppAddInput(app, master,
                                 (IswPointer)(intptr_t)IswInputReadMask,
                                 pty_input_cb, p);
    p->sigchld_id = IswAppAddSignal(app, sigchld_signal_cb, p);

    g_pty_for_signal = p;
    struct sigaction sa = {0};
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);

    return p;
}

void term_pty_write(TermPty *p, const char *data, size_t n)
{
    if (!p || p->fd < 0) return;
    while (n > 0) {
        ssize_t w = write(p->fd, data, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* drop — input outpacing child is rare for keyboard */
                return;
            }
            return;
        }
        data += w;
        n    -= (size_t)w;
    }
}

void term_pty_resize(TermPty *p, unsigned cols, unsigned rows,
                     unsigned px_w, unsigned px_h)
{
    if (!p || p->fd < 0) return;
    struct winsize ws = {
        .ws_row = rows, .ws_col = cols,
        .ws_xpixel = px_w, .ws_ypixel = px_h,
    };
    ioctl(p->fd, TIOCSWINSZ, &ws);
}

void term_pty_close(TermPty *p)
{
    if (!p) return;
    if (p->input_id) { IswRemoveInput(p->input_id); p->input_id = 0; }
    if (p->sigchld_id) { IswRemoveSignal(p->sigchld_id); p->sigchld_id = 0; }
    if (p->fd >= 0) { close(p->fd); p->fd = -1; }
    if (p->pid > 0) {
        kill(p->pid, SIGHUP);
        waitpid(p->pid, NULL, WNOHANG);
        p->pid = -1;
    }
    if (g_pty_for_signal == p) g_pty_for_signal = NULL;
    free(p);
}
