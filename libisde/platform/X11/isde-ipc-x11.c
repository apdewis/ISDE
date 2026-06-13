#define _POSIX_C_SOURCE 200809L
/*
 * isde-ipc-x11.c — X11 backend: ISDE IPC command-bus ops.
 *
 * Absorbs the body of the former isde-ipc.c. The _ISDE_COMMAND atom is interned
 * once at display open (IsdeDisplay.ipc_atom).
 */
#include "isde-platform-x11.h"

#include <string.h>

static int send(IsdeDisplay *d, uint32_t command,
                uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
    xcb_window_t root = d->screen->root;

    xcb_client_message_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = root;
    ev.type = d->ipc_atom;
    ev.format = 32;
    ev.data.data32[0] = command;
    ev.data.data32[1] = d0;
    ev.data.data32[2] = d1;
    ev.data.data32[3] = d2;
    ev.data.data32[4] = d3;

    xcb_send_event(d->conn, 0, root,
                   XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                   (const char *)&ev);
    xcb_flush(d->conn);
    return 1;
}

static int decode(IsdeDisplay *d, const IsdeEvent *ev, uint32_t *command,
                  uint32_t *d0, uint32_t *d1, uint32_t *d2, uint32_t *d3)
{
    const xcb_generic_event_t *ge = &ev->native;
    if ((ge->response_type & ~0x80) != XCB_CLIENT_MESSAGE) {
        return 0;
    }

    const xcb_client_message_event_t *cm =
        (const xcb_client_message_event_t *)ge;
    if (cm->type != d->ipc_atom) {
        return 0;
    }

    if (command) { *command = cm->data.data32[0]; }
    if (d0)      { *d0 = cm->data.data32[1]; }
    if (d1)      { *d1 = cm->data.data32[2]; }
    if (d2)      { *d2 = cm->data.data32[3]; }
    if (d3)      { *d3 = cm->data.data32[4]; }
    return 1;
}

const IsdePlatformIpcOps isde_x11_ipc_ops = {
    .send   = send,
    .decode = decode,
};
