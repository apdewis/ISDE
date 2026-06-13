#define _POSIX_C_SOURCE 200809L
/*
 * isde-startup-x11.c — X11 backend: startup-notification ops.
 *
 * Absorbs the _NET_STARTUP_INFO emission from the former isde-desktop.c. The
 * launcher (fork/exec, .desktop parsing) stays in isde-desktop.c; only the
 * wire-protocol emission lives here. Atoms are interned at display open.
 */
#include "isde-platform-x11.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static char *new_id(IsdeDisplay *d)
{
    char hostname[64];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        snprintf(hostname, sizeof(hostname), "localhost");
    }
    hostname[sizeof(hostname) - 1] = '\0';

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long seq = ++d->startup_seq;

    char buf[256];
    snprintf(buf, sizeof(buf), "%s+%d-%ld-%ld_TIME%lu",
             hostname, (int)getpid(), seq,
             (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000,
             (unsigned long)time(NULL));
    return strdup(buf);
}

static void send_message(IsdeDisplay *d, const char *msg)
{
    xcb_connection_t *conn = d->conn;
    xcb_window_t root = d->screen->root;
    xcb_atom_t begin_atom = d->startup_info_begin;
    xcb_atom_t cont_atom = d->startup_info;

    size_t len = strlen(msg) + 1;
    const char *p = msg;
    int first = 1;

    while (len > 0) {
        xcb_client_message_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.window = root;
        ev.type = first ? begin_atom : cont_atom;
        ev.format = 8;

        size_t chunk = len > 20 ? 20 : len;
        memcpy(ev.data.data8, p, chunk);

        xcb_send_event(conn, 0, root,
                       XCB_EVENT_MASK_PROPERTY_CHANGE,
                       (const char *)&ev);
        p += chunk;
        len -= chunk;
        first = 0;
    }
    xcb_flush(conn);
}

static char *quote_value(const char *val)
{
    if (!val) {
        return strdup("\"\"");
    }

    int need_quote = 0;
    for (const char *p = val; *p; p++) {
        if (*p == ' ' || *p == '"' || *p == '\\') {
            need_quote = 1;
            break;
        }
    }

    if (!need_quote) {
        return strdup(val);
    }

    size_t len = strlen(val);
    char *out = malloc(len * 2 + 3);
    char *o = out;
    *o++ = '"';
    for (const char *p = val; *p; p++) {
        if (*p == '"' || *p == '\\') {
            *o++ = '\\';
        }
        *o++ = *p;
    }
    *o++ = '"';
    *o = '\0';
    return out;
}

static void send_new(IsdeDisplay *d, const char *id, const char *name,
                     const char *bin, const char *wm_class)
{
    char *qname = quote_value(name);
    char *qbin = quote_value(bin);
    char *qwmclass = quote_value(wm_class);

    char msg[1024];
    int pos = snprintf(msg, sizeof(msg),
                       "new: ID=%s NAME=%s SCREEN=%d",
                       id, qname, d->screen_num);
    if (bin) {
        pos += snprintf(msg + pos, sizeof(msg) - pos, " BIN=%s", qbin);
    }
    if (wm_class) {
        pos += snprintf(msg + pos, sizeof(msg) - pos, " WMCLASS=%s", qwmclass);
    }
    pos += snprintf(msg + pos, sizeof(msg) - pos, " TIMESTAMP=%lu",
                    (unsigned long)time(NULL));

    free(qname);
    free(qbin);
    free(qwmclass);

    send_message(d, msg);
}

static void send_remove(IsdeDisplay *d, const char *id)
{
    char msg[256];
    snprintf(msg, sizeof(msg), "remove: ID=%s", id);
    send_message(d, msg);
}

const IsdePlatformStartupOps isde_x11_startup_ops = {
    .new_id      = new_id,
    .send_new    = send_new,
    .send_remove = send_remove,
};
