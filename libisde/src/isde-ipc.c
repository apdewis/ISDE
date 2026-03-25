/*
 * isde-ipc.c — X11 ClientMessage IPC helpers
 */
#include "isde/isde-ipc.h"

#include <stdlib.h>
#include <string.h>
#include <xcb/xcb_aux.h>

struct IsdeIpc {
    xcb_connection_t *conn;
    xcb_window_t      root;
    xcb_atom_t        atom;
};

static xcb_atom_t intern_atom(xcb_connection_t *conn, const char *name)
{
    xcb_intern_atom_cookie_t cookie =
        xcb_intern_atom(conn, 0, strlen(name), name);
    xcb_intern_atom_reply_t *reply =
        xcb_intern_atom_reply(conn, cookie, NULL);
    if (!reply)
        return XCB_ATOM_NONE;
    xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

IsdeIpc *isde_ipc_init(xcb_connection_t *conn, int screen)
{
    IsdeIpc *ipc = calloc(1, sizeof(*ipc));
    if (!ipc)
        return NULL;

    ipc->conn = conn;

    xcb_screen_t *scr = xcb_aux_get_screen(conn, screen);
    if (!scr) {
        free(ipc);
        return NULL;
    }
    ipc->root = scr->root;

    ipc->atom = intern_atom(conn, ISDE_IPC_ATOM);
    if (ipc->atom == XCB_ATOM_NONE) {
        free(ipc);
        return NULL;
    }

    return ipc;
}

void isde_ipc_free(IsdeIpc *ipc)
{
    free(ipc);
}

int isde_ipc_send(IsdeIpc *ipc, uint32_t command,
                  uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
    xcb_client_message_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = ipc->root;
    ev.type = ipc->atom;
    ev.format = 32;
    ev.data.data32[0] = command;
    ev.data.data32[1] = d0;
    ev.data.data32[2] = d1;
    ev.data.data32[3] = d2;
    ev.data.data32[4] = d3;

    xcb_send_event(ipc->conn, 0, ipc->root,
                   XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                   (const char *)&ev);
    xcb_flush(ipc->conn);
    return 1;
}

int isde_ipc_decode(IsdeIpc *ipc, xcb_generic_event_t *ev,
                    uint32_t *command,
                    uint32_t *d0, uint32_t *d1, uint32_t *d2, uint32_t *d3)
{
    if ((ev->response_type & ~0x80) != XCB_CLIENT_MESSAGE)
        return 0;

    xcb_client_message_event_t *cm = (xcb_client_message_event_t *)ev;
    if (cm->type != ipc->atom)
        return 0;

    if (command) *command = cm->data.data32[0];
    if (d0)      *d0 = cm->data.data32[1];
    if (d1)      *d1 = cm->data.data32[2];
    if (d2)      *d2 = cm->data.data32[3];
    if (d3)      *d3 = cm->data.data32[4];
    return 1;
}
