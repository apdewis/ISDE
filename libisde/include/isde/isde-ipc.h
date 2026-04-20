/*
 * isde-ipc.h — X11 ClientMessage IPC helpers
 *
 * Simple mechanism for ISDE components to send commands to each other
 * via X11 ClientMessage events on the root window.
 */
#ifndef ISDE_IPC_H
#define ISDE_IPC_H

#include <xcb/xcb.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ISDE IPC message type atom name.  Interned once per connection. */
#define ISDE_IPC_ATOM "_ISDE_COMMAND"

typedef struct IsdeIpc IsdeIpc;

/* Initialize IPC on the given connection.  Interns the ISDE_IPC_ATOM. */
IsdeIpc *isde_ipc_init(xcb_connection_t *conn, int screen);
void     isde_ipc_free(IsdeIpc *ipc);

/* Send a command to the root window.  data is up to 4 uint32 values
 * whose meaning is defined by the command. */
int isde_ipc_send(IsdeIpc *ipc, uint32_t command,
                  uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3);

/* Check if an XCB event is an ISDE IPC message.
 * Returns 1 and fills out command/data fields if so, 0 otherwise. */
int isde_ipc_decode(IsdeIpc *ipc, xcb_generic_event_t *ev,
                    uint32_t *command,
                    uint32_t *d0, uint32_t *d1, uint32_t *d2, uint32_t *d3);

/* Well-known command IDs */
#define ISDE_CMD_QUIT       1  /* Request component to exit cleanly */
#define ISDE_CMD_RELOAD     2  /* Request config reload */
#define ISDE_CMD_LOGOUT     3  /* Session logout */
#define ISDE_CMD_LOCK       4  /* Request screen lock */
#define ISDE_CMD_SHUTDOWN   5  /* Request shutdown (with confirmation) */
#define ISDE_CMD_REBOOT     6  /* Request reboot (with confirmation) */
#define ISDE_CMD_SUSPEND    7  /* Request suspend */
#define ISDE_CMD_TOGGLE_START_MENU 8  /* Toggle panel start menu */

#ifdef __cplusplus
}
#endif

#endif /* ISDE_IPC_H */
