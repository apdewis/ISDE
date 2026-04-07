#define _POSIX_C_SOURCE 200809L
/*
 * ipc.c — greeter-side IPC (client connection to daemon)
 */
#include "greeter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

int greeter_ipc_init(Greeter *g)
{
    const char *sock_path = getenv("ISDE_DM_SOCK");
    if (!sock_path || !*sock_path) {
        fprintf(stderr, "isde-greeter: ISDE_DM_SOCK not set\n");
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "isde-greeter: socket: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "isde-greeter: connect %s: %s\n",
                sock_path, strerror(errno));
        close(fd);
        return -1;
    }

    g->ipc_fd = fd;
    g->ipc_buf_len = 0;

    fprintf(stderr, "isde-greeter: connected to daemon at %s\n", sock_path);
    return 0;
}

void greeter_ipc_cleanup(Greeter *g)
{
    if (g->ipc_fd >= 0) {
        close(g->ipc_fd);
        g->ipc_fd = -1;
    }
}

int greeter_ipc_send(Greeter *g, const char *msg)
{
    if (g->ipc_fd < 0) {
        return -1;
    }

    size_t len = strlen(msg);
    char buf[4096];
    if (len + 1 > sizeof(buf)) {
        return -1;
    }
    memcpy(buf, msg, len);
    buf[len] = '\n';

    ssize_t total = len + 1;
    ssize_t written = 0;
    while (written < total) {
        ssize_t n = write(g->ipc_fd, buf + written, total - written);
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
