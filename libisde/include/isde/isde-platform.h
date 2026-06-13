/*
 * isde-platform.h — ISDE platform-abstraction layer (neutral API)
 *
 * ISDE's desktop-environment protocols (EWMH, monitor/display config, DPMS, the
 * ISDE IPC command bus, startup notification, RESOURCE_MANAGER publishing) are
 * platform-dependent. This header is the neutral interface to them; a platform
 * backend (X11 today) implements the IsdePlatformOps vtable. No xcb/X11 type
 * appears in any signature here — the backend maps the opaque handles to native
 * types internally.
 *
 * Connection ownership: the platform layer ALWAYS owns its own server
 * connection. A display is obtained from isde_platform_open(); the layer opens
 * and owns the connection. Components never call xcb_connect and never see an
 * xcb_connection_t. 
 *
 * See ISDE_PLATFORM_PLAN.md for the architecture.
 */
#ifndef ISDE_PLATFORM_H
#define ISDE_PLATFORM_H

#include <stdint.h>
#include <stddef.h>

/* Opaque display handle: the platform layer's connection + screen/root. */
typedef struct IsdeDisplay IsdeDisplay;

/* Opaque neutral event, returned by isde_display_poll_event(). The backend
   translates its native event into this; free with free(). */
typedef struct IsdeEvent IsdeEvent;

/* Window id as a portable value. On X11 this is the xcb_window_t numeric id;
   the backend reinterprets it. 0 means "none". */
typedef uint32_t IsdeWindow;

/* Atom id as a portable value (X11-numeric, carried opaquely). 0 = none. */
typedef uint32_t IsdeAtom;

#define ISDE_WINDOW_NONE ((IsdeWindow)0)
#define ISDE_ATOM_NONE   ((IsdeAtom)0)

/* Monitor geometry in physical pixels. (Was IsdeMonitor in isde-randr.h.) */
typedef struct {
    int16_t  x, y;
    uint16_t width, height;
} IsdeMonitor;

/* ---------- ISDE IPC command bus ---------- */

/* IPC message type atom name. */
#define ISDE_IPC_ATOM "_ISDE_COMMAND"

/* Well-known command IDs. */
#define ISDE_CMD_QUIT              1  /* Request component to exit cleanly */
#define ISDE_CMD_RELOAD            2  /* Request config reload */
#define ISDE_CMD_LOGOUT            3  /* Session logout */
#define ISDE_CMD_LOCK              4  /* Request screen lock */
#define ISDE_CMD_SHUTDOWN          5  /* Request shutdown (with confirmation) */
#define ISDE_CMD_REBOOT            6  /* Request reboot (with confirmation) */
#define ISDE_CMD_SUSPEND           7  /* Request suspend */
#define ISDE_CMD_TOGGLE_START_MENU 8  /* Toggle panel start menu */

/*
 * =================================================================
 * Sub-vtables
 * =================================================================
 */

/* EWMH / ICCCM property semantics. All operate on the display's screen/root
   except where a window is named explicitly. */
typedef struct {
    int      (*set_supported)(IsdeDisplay *d, const IsdeAtom *atoms, int count);
    int      (*set_client_list)(IsdeDisplay *d, const IsdeWindow *wins, int count);
    int      (*set_client_list_stacking)(IsdeDisplay *d, const IsdeWindow *wins, int count);
    int      (*set_active_window)(IsdeDisplay *d, IsdeWindow win);
    int      (*set_number_of_desktops)(IsdeDisplay *d, uint32_t n);
    int      (*set_current_desktop)(IsdeDisplay *d, uint32_t desk);
    int      (*set_wm_name)(IsdeDisplay *d, IsdeWindow win, const char *name);

    IsdeWindow (*get_active_window)(IsdeDisplay *d);
    uint32_t (*get_current_desktop)(IsdeDisplay *d);
    uint32_t (*get_number_of_desktops)(IsdeDisplay *d);
    /* Caller free()s *wins. Returns count. */
    int      (*get_client_list)(IsdeDisplay *d, IsdeWindow **wins);
    int      (*get_client_list_stacking)(IsdeDisplay *d, IsdeWindow **wins);
    IsdeAtom (*get_window_type)(IsdeDisplay *d, IsdeWindow win);
    /* Caller free()s *instance and *class. Returns 1 on success. */
    int      (*get_wm_class)(IsdeDisplay *d, IsdeWindow win,
                             char **instance_out, char **class_out);
    /* Returns 1 (workarea or screen fallback). */
    int      (*get_workarea)(IsdeDisplay *d, int *x, int *y, int *w, int *h);
    uint32_t (*get_wm_desktop)(IsdeDisplay *d, IsdeWindow win);

    void     (*request_active_window)(IsdeDisplay *d, IsdeWindow win);
    void     (*request_close_window)(IsdeDisplay *d, IsdeWindow win);
    void     (*request_current_desktop)(IsdeDisplay *d, uint32_t desktop);
    void     (*request_wm_desktop)(IsdeDisplay *d, IsdeWindow win, uint32_t desktop);

    void     (*set_desktop_layout)(IsdeDisplay *d, int orientation,
                                   int cols, int rows, int starting_corner);
    int      (*get_desktop_layout)(IsdeDisplay *d, int *orientation,
                                   int *cols, int *rows, int *starting_corner);
} IsdePlatformEwmhOps;

/* Monitor enumeration (query side — used by all display-aware components). */
typedef struct {
    /* Primary monitor geometry. Returns 1 if a monitor was found, 0 on
       full-screen fallback (out always filled). */
    int (*primary)(IsdeDisplay *d, IsdeMonitor *out);
    /* Monitor containing (px, py). Returns 1 if found, 0 on fallback. */
    int (*monitor_at)(IsdeDisplay *d, int px, int py, IsdeMonitor *out);
    /* All active monitors. Allocates *out (caller free()s). Returns count. */
    int (*monitors)(IsdeDisplay *d, IsdeMonitor **out);
} IsdePlatformDisplayOps;

/* Display configuration (settings + displayd only). Opaque output/mode handles
   are uint32 ids (X11-numeric, carried opaquely). */
typedef struct {
    /* Compute refresh rate (Hz) for a mode id, or 0.0 if unknown. */
    double (*mode_refresh)(IsdeDisplay *d, uint32_t mode_id);
    /* Enumerate outputs: allocates *ids (caller free()s); returns count. */
    int    (*outputs)(IsdeDisplay *d, uint32_t **ids);
    /* TODO sub-ops for set-mode / set-primary / set-screen-size / EDID /
       hotplug are added as displayd/settings are ported (Phase 3/4). The
       struct exists now so the vtable shape is stable. */
} IsdePlatformDisplayConfigOps;

/* Display power (DPMS). Timeouts in seconds. Returns 0 on success. */
typedef struct {
    int (*get_timeouts)(IsdeDisplay *d, int *standby, int *suspend, int *off);
    int (*set_timeouts)(IsdeDisplay *d, int standby, int suspend, int off);
} IsdePlatformPowerOps;

/* ISDE IPC command bus (root-window ClientMessage broadcast). */
typedef struct {
    /* Send a command to root. data is up to 4 uint32 values. Returns 1. */
    int (*send)(IsdeDisplay *d, uint32_t command,
                uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3);
    /* Decode a polled IsdeEvent. Returns 1 and fills fields if it is an ISDE
       command message, 0 otherwise. */
    int (*decode)(IsdeDisplay *d, const IsdeEvent *ev, uint32_t *command,
                  uint32_t *d0, uint32_t *d1, uint32_t *d2, uint32_t *d3);
} IsdePlatformIpcOps;

/* Startup notification (_NET_STARTUP_INFO). */
typedef struct {
    /* Allocate a fresh startup id (caller free()s). */
    char *(*new_id)(IsdeDisplay *d);
    /* Emit a "new:" message for a launch. name/bin/wm_class may be NULL. */
    void  (*send_new)(IsdeDisplay *d, const char *id, const char *name,
                      const char *bin, const char *wm_class);
    /* Emit a "remove:" message ending the sequence `id`. */
    void  (*send_remove)(IsdeDisplay *d, const char *id);
} IsdePlatformStartupOps;

/* Root-window publishing (RESOURCE_MANAGER). */
typedef struct {
    /* Replace the root RESOURCE_MANAGER property with `rdb` (length bytes). */
    void (*set_resource_manager)(IsdeDisplay *d, const char *rdb, size_t length);
} IsdePlatformRootOps;

/* Top-level vtable: one const instance per backend. */
typedef struct {
    const IsdePlatformEwmhOps          *ewmh;
    const IsdePlatformDisplayOps       *display;
    const IsdePlatformDisplayConfigOps *display_config;
    const IsdePlatformPowerOps         *power;
    const IsdePlatformIpcOps           *ipc;
    const IsdePlatformStartupOps       *startup;
    const IsdePlatformRootOps          *root;
} IsdePlatformOps;

/*
 * =================================================================
 * Lifecycle and the active backend
 * =================================================================
 */

/* The active backend's ops (selected once). Never NULL. */
const IsdePlatformOps *isde_platform(void);

/* Open a display: the layer opens and owns the server connection.
   display_name NULL = default ($DISPLAY). Returns NULL on failure. */
IsdeDisplay *isde_platform_open(const char *display_name);

/* Close and free a display opened by isde_platform_open(). */
void isde_display_close(IsdeDisplay *d);

/* Event-loop file descriptor for poll()/select(). */
int isde_display_event_fd(IsdeDisplay *d);

/* Next pending event, reading the socket if needed; NULL if none.
   Caller frees with free(). */
IsdeEvent *isde_display_poll_event(IsdeDisplay *d);

/* Flush buffered requests to the server. */
void isde_display_flush(IsdeDisplay *d);

/* The root window of the display's default screen. */
IsdeWindow isde_display_root(IsdeDisplay *d);

/* Convenience: clamp *w/*h so the window fits the current EWMH workarea. */
void isde_clamp_to_workarea(IsdeDisplay *d, int *w, int *h);

#endif /* ISDE_PLATFORM_H */
