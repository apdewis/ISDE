/*
 * tray-net.h — isde-tray-net internal header
 */
#ifndef ISDE_TRAY_NET_H
#define ISDE_TRAY_NET_H

#include <ISW/Intrinsic.h>
#include <ISW/StringDefs.h>
#include <ISW/Shell.h>
#include <ISW/Command.h>
#include <ISW/Toggle.h>
#include <ISW/Label.h>
#include <ISW/Form.h>
#include <ISW/FlexBox.h>
#include <ISW/Box.h>
#include <ISW/Viewport.h>
#include <ISW/ListBox.h>
#include <ISW/ListBoxRow.h>
#include <ISW/IswTrayIcon.h>
#include <ISW/ISWSVG.h>

#include <xcb/xcb.h>

#include <dbus/dbus.h>

#include "isde/isde-dbus.h"
#include "isde/isde-dialog.h"
#include "isde/isde-theme.h"

/* ---------- D-Bus constants ---------- */

#define CONNMAN_SERVICE       "net.connman"
#define CONNMAN_MANAGER_PATH  "/"
#define CONNMAN_MANAGER_IFACE "net.connman.Manager"
#define CONNMAN_SERVICE_IFACE "net.connman.Service"
#define CONNMAN_TECH_IFACE    "net.connman.Technology"
#define CONNMAN_AGENT_IFACE   "net.connman.Agent"
#define CONNMAN_AGENT_PATH    "/org/isde/TrayNetAgent"

/* ---------- limits ---------- */

#define MAX_SERVICES      64
#define MAX_TECHNOLOGIES   8
#define NAME_LEN         256
#define PATH_LEN         512

/* ---------- ConnMan state ---------- */

typedef struct TechInfo {
    char    path[PATH_LEN];
    char    name[NAME_LEN];
    char    type[64];
    int     powered;
    int     connected;
} TechInfo;

typedef struct ServiceInfo {
    char    path[PATH_LEN];
    char    name[NAME_LEN];
    char    type[64];
    char    state[64];
    char    security[64];
    char    error[NAME_LEN];
    uint8_t strength;
    int     favorite;
    int     autoconnect;
} ServiceInfo;

/* ---------- Icon states ---------- */

enum {
    ICON_DISCONNECTED = 0,
    ICON_WIRED,
    ICON_WIFI_WEAK,
    ICON_WIFI_OK,
    ICON_WIFI_GOOD,
    ICON_WIFI_EXCELLENT,
    ICON_COUNT
};

/* ---------- Tray applet state ---------- */

typedef struct TrayNet {
    IswAppContext       app;
    Widget              toplevel;

    IswTrayIcon         tray_icon;
    int                 icon_state;

    /* Popup */
    Widget              popup_shell;
    Widget              popup_outer;
    Widget              popup_viewport;
    Widget              popup_listbox;
    int                 popup_visible;

    /* ConnMan state */
    TechInfo            techs[MAX_TECHNOLOGIES];
    int                 ntechs;
    ServiceInfo         services[MAX_SERVICES];
    int                 nservices;
    char                manager_state[64];

    /* D-Bus */
    DBusConnection     *system_bus;
    IsdeDBus           *session_dbus;

    /* ConnMan availability */
    int                 connman_available;

    /* Agent */
    DBusMessage        *pending_agent_req;
    Widget              agent_dialog;

    int                 running;
    int                 restart;
} TrayNet;

/* ---------- tray-net.c ---------- */
int  tray_net_init(TrayNet *tn, int *argc, char **argv);
void tray_net_run(TrayNet *tn);
void tray_net_cleanup(TrayNet *tn);
void tray_net_update_icon(TrayNet *tn);

/* ---------- connman.c ---------- */
int  tn_connman_init(TrayNet *tn);
void tn_connman_cleanup(TrayNet *tn);
int  tn_connman_refresh(TrayNet *tn);
int  tn_connman_get_technologies(TrayNet *tn);
int  tn_connman_get_services(TrayNet *tn);
int  tn_connman_get_state(TrayNet *tn);
int  tn_connman_service_connect(TrayNet *tn, const char *path);
int  tn_connman_service_disconnect(TrayNet *tn, const char *path);
int  tn_connman_tech_set_powered(TrayNet *tn, const char *path, int powered);
int  tn_connman_scan(TrayNet *tn, const char *tech_path);

/* ---------- agent.c ---------- */
int  tn_agent_init(TrayNet *tn);
void tn_agent_cleanup(TrayNet *tn);

/* ---------- menu.c ---------- */
void tn_menu_init(TrayNet *tn);
void tn_menu_show(TrayNet *tn);
void tn_menu_hide(TrayNet *tn);
void tn_menu_rebuild(TrayNet *tn);
void tn_menu_cleanup(TrayNet *tn);

#endif /* ISDE_TRAY_NET_H */
