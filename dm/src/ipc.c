#define _POSIX_C_SOURCE 200809L
/*
 * ipc.c — Unix domain socket IPC between daemon and greeter
 */
#include "dm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "isde/isde-desktop.h"

int dm_ipc_init(Dm *dm)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/greeter.sock", dm->plat->rundir);

    /* Remove stale socket */
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    fcntl(fd, F_SETFD, FD_CLOEXEC);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    /* Restrict access: root + isde-dm group */
    chmod(path, 0660);

    if (listen(fd, 1) < 0) {
        close(fd);
        unlink(path);
        return -1;
    }

    dm->ipc_listen_fd = fd;
    dm->ipc_client_fd = -1;
    dm->ipc_buf_len = 0;

    fprintf(stderr, "isde-dm: IPC listening on %s\n", path);
    return 0;
}

void dm_ipc_cleanup(Dm *dm)
{
    if (dm->ipc_client_fd >= 0) {
        close(dm->ipc_client_fd);
        dm->ipc_client_fd = -1;
    }
    if (dm->ipc_listen_fd >= 0) {
        close(dm->ipc_listen_fd);
        dm->ipc_listen_fd = -1;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/greeter.sock", dm->plat->rundir);
    unlink(path);
}

void dm_ipc_accept(Dm *dm)
{
    int fd = accept(dm->ipc_listen_fd, NULL, NULL);
    if (fd < 0) {
        return;
    }

    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, O_NONBLOCK);

    /* Only one greeter connection at a time */
    if (dm->ipc_client_fd >= 0) {
        close(dm->ipc_client_fd);
    }

    dm->ipc_client_fd = fd;
    dm->ipc_buf_len = 0;
    fprintf(stderr, "isde-dm: greeter connected\n");
}

int dm_ipc_send(Dm *dm, const char *msg)
{
    if (dm->ipc_client_fd < 0) {
        return -1;
    }

    size_t len = strlen(msg);
    char buf[DM_IPC_MAX_MSG];
    if (len + 1 > sizeof(buf)) {
        return -1;
    }
    memcpy(buf, msg, len);
    buf[len] = '\n';

    ssize_t written = 0;
    ssize_t total = len + 1;
    while (written < total) {
        ssize_t n = write(dm->ipc_client_fd, buf + written, total - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        written += n;
    }
    return 0;
}

/* ---------- message dispatch ---------- */

static void handle_auth(Dm *dm, const char *args)
{
    /* Parse: "AUTH <username> <password>" */
    char username[DM_IPC_MAX_USER];
    char password[DM_IPC_MAX_PASS];

    const char *space = strchr(args, ' ');
    if (!space) {
        dm_ipc_send(dm, "AUTH_FAIL Invalid request");
        return;
    }

    size_t ulen = space - args;
    if (ulen >= sizeof(username)) {
        dm_ipc_send(dm, "AUTH_FAIL Username too long");
        return;
    }
    memcpy(username, args, ulen);
    username[ulen] = '\0';

    const char *pass = space + 1;
    size_t plen = strlen(pass);
    if (plen >= sizeof(password)) {
        dm_ipc_send(dm, "AUTH_FAIL Password too long");
        return;
    }
    memcpy(password, pass, plen);
    password[plen] = '\0';

    char errbuf[256];
    if (dm_auth_check(username, password, errbuf, sizeof(errbuf)) != 0) {
        char reply[512];
        snprintf(reply, sizeof(reply), "AUTH_FAIL %s", errbuf);
        dm_ipc_send(dm, reply);
        memset(password, 0, sizeof(password));
        return;
    }

    memset(password, 0, sizeof(password));

    /* Determine which session to start */
    const char *desktop = dm->session_desktop;
    if (!desktop) {
        desktop = dm->default_session;
    }

    fprintf(stderr, "isde-dm: auth success for '%s'\n", username);
    dm_ipc_send(dm, "AUTH_OK");

    if (dm->locked) {
        /* Lock screen: verify the user matches, then unlock */
        if (dm->session_user && strcmp(username, dm->session_user) == 0) {
            dm_unlock_session(dm);
        }
    } else {
        /* Normal login: stop greeter and start session */
        dm_greeter_stop(dm);
        dm_session_start(dm, username, desktop);
    }
}

static void handle_session(Dm *dm, const char *args)
{
    free(dm->session_desktop);
    dm->session_desktop = strdup(args);
    fprintf(stderr, "isde-dm: session set to '%s'\n", args);
}

static void handle_list_sessions(Dm *dm)
{
    /* Scan /usr/share/xsessions/ */
    int count = 0;
    IsdeDesktopEntry **entries =
        isde_desktop_scan_dir("/usr/share/xsessions", &count);

    char buf[DM_IPC_MAX_MSG];
    int off = snprintf(buf, sizeof(buf), "SESSIONS %d\n", count);

    for (int i = 0; i < count && off < (int)sizeof(buf) - 128; i++) {
        const char *name = isde_desktop_name(entries[i]);
        const char *file = isde_desktop_exec(entries[i]);
        if (name && file) {
            off += snprintf(buf + off, sizeof(buf) - off, "%s\t%s\n",
                           name, file);
        }
    }

    /* Remove trailing newline for clean send */
    if (off > 0 && buf[off - 1] == '\n') {
        buf[off - 1] = '\0';
    }

    dm_ipc_send(dm, buf);

    for (int i = 0; i < count; i++) {
        isde_desktop_free(entries[i]);
    }
    free(entries);
}

static void dispatch_message(Dm *dm, const char *line)
{
    if (strncmp(line, "AUTH ", 5) == 0) {
        handle_auth(dm, line + 5);
    } else if (strncmp(line, "SESSION ", 8) == 0) {
        handle_session(dm, line + 8);
    } else if (strcmp(line, "LIST_SESSIONS") == 0) {
        handle_list_sessions(dm);
    } else if (strcmp(line, "SHUTDOWN") == 0) {
        if (dm->allow_shutdown) {
            dm_power_shutdown(dm);
        }
    } else if (strcmp(line, "REBOOT") == 0) {
        if (dm->allow_reboot) {
            dm_power_reboot(dm);
        }
    } else if (strcmp(line, "SUSPEND") == 0) {
        if (dm->allow_suspend) {
            dm_power_suspend(dm);
        }
    } else if (strcmp(line, "CANCEL_LOCK") == 0) {
        dm_unlock_session(dm);
    } else {
        fprintf(stderr, "isde-dm: unknown IPC message: %s\n", line);
    }
}

void dm_ipc_handle(Dm *dm)
{
    if (dm->ipc_client_fd < 0) {
        return;
    }

    int space = DM_IPC_MAX_MSG - dm->ipc_buf_len - 1;
    if (space <= 0) {
        /* Buffer overflow — drop connection */
        close(dm->ipc_client_fd);
        dm->ipc_client_fd = -1;
        dm->ipc_buf_len = 0;
        return;
    }

    ssize_t n = read(dm->ipc_client_fd,
                     dm->ipc_buf + dm->ipc_buf_len, space);
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
            close(dm->ipc_client_fd);
            dm->ipc_client_fd = -1;
            dm->ipc_buf_len = 0;
            fprintf(stderr, "isde-dm: greeter disconnected\n");
        }
        return;
    }

    dm->ipc_buf_len += n;
    dm->ipc_buf[dm->ipc_buf_len] = '\0';

    /* Process complete lines */
    char *start = dm->ipc_buf;
    char *nl;
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        if (nl > start) {
            dispatch_message(dm, start);
        }
        start = nl + 1;
    }

    /* Move remaining partial data to front of buffer */
    int remaining = dm->ipc_buf_len - (start - dm->ipc_buf);
    if (remaining > 0 && start != dm->ipc_buf) {
        memmove(dm->ipc_buf, start, remaining);
    }
    dm->ipc_buf_len = remaining;
}
