#define _POSIX_C_SOURCE 200809L
/*
 * instance.c — single-instance detection and path forwarding
 *
 * Uses an X selection (_ISDE_FM_INSTANCE) for instance detection.
 * If another instance owns the selection, sends the requested path
 * via a property + ClientMessage and returns.  The owning instance
 * watches for the message and opens a new window.
 */
#include "fm.h"

#include <ISW/ISWPlatform.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Atom atom_instance;   /* _ISDE_FM_INSTANCE */
static Atom atom_open_path;  /* _ISDE_FM_OPEN_PATH */

/* ---------- owning instance: handle incoming open-path request ---------- */

static void instance_event_handler(Widget w, IswPointer closure,
                                   IswEvent *ev, Boolean *cont)
{
    (void)cont;
    FmApp *app = (FmApp *)closure;

    if (ev->kind != IswProtocol)
        return;

    if (ev->protocol.message_type != (IswProtocolId)atom_open_path)
        return;

    IswDisplay dpy = IswDisplayOf(w);
    IswWindow win = _IswPlatformWidgetWindow(dpy, w);
    IswProperty prop = {0};
    if (!_IswPlatformGetProperty(dpy, win, atom_open_path,
                                 ISW_ATOM_STRING, 0, 4096, &prop)) {
        return;
    }
    _IswPlatformDeleteProperty(dpy, win, atom_open_path);

    if (prop.num_items > 0 && prop.value) {
        char *path = malloc(prop.num_items + 1);
        memcpy(path, prop.value, prop.num_items);
        path[prop.num_items] = '\0';
        const char *open_path = path;
        if (strncmp(open_path, "file://", 7) == 0) {
            open_path += 7;
            if (open_path[0] == '\0')
                open_path = "/";
        }
        fm_window_new(app, open_path);
        free(path);
    }
    _IswPlatformFreeProperty(&prop);
}

/* ---------- selection convert (we are the owner) ---------- */

static Boolean instance_convert(Widget w, Atom *selection, Atom *target,
                                Atom *type_return, IswPointer *value_return,
                                unsigned long *length_return, int *format_return)
{
    (void)selection;
    if (*target == atom_instance) {
        IswDisplay dpy = IswDisplayOf(w);
        IswWindow win = _IswPlatformWidgetWindow(dpy, w);
        static uint32_t win_id;
        win_id = _IswPlatformWindowId(win);
        *type_return = IswDndInternType(w, "CARDINAL");
        *value_return = (IswPointer)&win_id;
        *length_return = 1;
        *format_return = 32;
        return True;
    }
    return False;
}

static void instance_lose(Widget w, Atom *selection)
{
    (void)w; (void)selection;
}

/* ---------- public API ---------- */

/*
 * Returns: 1 if we are the primary instance (selection owned, handler installed)
 *          0 if another instance exists (path forwarded, caller should exit)
 *         -1 on error
 */
int instance_try_primary(FmApp *app, const char *path)
{
    Widget shell = app->first_toplevel;
    IswDisplay dpy = IswDisplayOf(shell);

    atom_instance  = IswDndInternType(shell, "_ISDE_FM_INSTANCE");
    atom_open_path = IswDndInternType(shell, "_ISDE_FM_OPEN_PATH");
    if (atom_instance == None || atom_open_path == None) {
        return -1;
    }

    if (!IswIsRealized(shell)) {
        IswRealizeWidget(shell);
    }

    IswWindow our_win = _IswPlatformWidgetWindow(dpy, shell);

    IswWindow owner = _IswPlatformGetSelectionOwner(dpy, atom_instance);

    if (!owner) {
        _IswPlatformSetSelectionOwner(dpy, our_win, atom_instance, 0);
        _IswPlatformFlush(dpy);

        IswWindow verify = _IswPlatformGetSelectionOwner(dpy, atom_instance);
        if (verify != our_win) {
            goto forward;
        }

        IswAddEventHandler(shell, (EventMask)0, True,
                          instance_event_handler, app);
        IswOwnSelection(shell, atom_instance, 0,
                        instance_convert, instance_lose, NULL);
        return 1;
    }

forward:
    _IswPlatformChangeProperty(dpy, owner, atom_open_path,
                               ISW_ATOM_STRING, 8, ISW_PROP_MODE_REPLACE,
                               path, (uint32_t)strlen(path));

    uint32_t our_id = _IswPlatformWindowId(our_win);
    uint8_t data[20];
    memset(data, 0, sizeof(data));
    memcpy(data, &our_id, sizeof(our_id));

    _IswPlatformSendMessage(dpy, owner, owner, atom_open_path,
                            32, data, False, 0);
    _IswPlatformFlush(dpy);

    return 0;
}
