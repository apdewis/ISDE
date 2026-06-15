/*
 * tray-net.h — network tray module (ConnMan) for isde-panel
 */
#ifndef ISDE_TRAY_NET_H
#define ISDE_TRAY_NET_H

#include "panel.h"

#include <ISW/Toggle.h>
#include <ISW/ListBox.h>
#include <ISW/ListBoxRow.h>

#include <dbus/dbus.h>

/* ---------- D-Bus constants ---------- */

#define CONNMAN_SERVICE       "net.connman"
#define CONNMAN_MANAGER_PATH  "/"
#define CONNMAN_MANAGER_IFACE "net.connman.Manager"
#define CONNMAN_SERVICE_IFACE "net.connman.Service"
#define CONNMAN_TECH_IFACE    "net.connman.Technology"
#define CONNMAN_AGENT_IFACE   "net.connman.Agent"
#define CONNMAN_AGENT_PATH    "/org/isde/TrayNetAgent"

/* ---------- limits ---------- */

#define TN_MAX_SERVICES      64
#define TN_MAX_TECHNOLOGIES   8
#define TN_NAME_LEN         256
#define TN_PATH_LEN         512

/* ---------- ConnMan state ---------- */

typedef struct TechInfo {
    char    path[TN_PATH_LEN];
    char    name[TN_NAME_LEN];
    char    type[64];
    int     powered;
    int     connected;
} TechInfo;

typedef struct ServiceInfo {
    char    path[TN_PATH_LEN];
    char    name[TN_NAME_LEN];
    char    type[64];
    char    state[64];
    char    security[64];
    char    interface[64];
    char    error[TN_NAME_LEN];
    uint8_t strength;
    int     favorite;
    int     autoconnect;
} ServiceInfo;

/* ---------- Icon states ---------- */

enum {
    TN_ICON_DISCONNECTED = 0,
    TN_ICON_WIRED,
    TN_ICON_WIFI_WEAK,
    TN_ICON_WIFI_OK,
    TN_ICON_WIFI_GOOD,
    TN_ICON_WIFI_EXCELLENT,
    TN_ICON_COUNT
};

/* ---------- Tray module state ---------- */

typedef struct TrayNet {
    struct Panel   *panel;

    Widget          icon;
    int             icon_state;

    /* Popup */
    Widget          popup_shell;
    Widget          popup_outer;
    Widget          popup_viewport;
    Widget          popup_listbox;
    int             popup_visible;

    /* ConnMan state */
    TechInfo        techs[TN_MAX_TECHNOLOGIES];
    int             ntechs;
    ServiceInfo     services[TN_MAX_SERVICES];
    int             nservices;
    char            manager_state[64];

    /* D-Bus (system bus — ConnMan lives here) */
    DBusConnection *system_bus;

    /* ConnMan availability */
    int             connman_available;

    /* Agent */
    DBusMessage    *pending_agent_req;
    Widget          agent_dialog;
} TrayNet;

/* ---------- tray-net.c ---------- */
void tn_net_init(struct Panel *p);
void tn_net_cleanup(struct Panel *p);
void tn_net_update_icon(TrayNet *tn);
void tn_net_reload_theme(TrayNet *tn);

/* ---------- tray-net-connman.c ---------- */
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

/* ---------- tray-net-agent.c ---------- */
int  tn_agent_init(TrayNet *tn);
void tn_agent_cleanup(TrayNet *tn);

/* ---------- tray-net-menu.c ---------- */
void tn_menu_init(TrayNet *tn);
void tn_menu_show(TrayNet *tn);
void tn_menu_hide(TrayNet *tn);
void tn_menu_rebuild(TrayNet *tn);
void tn_menu_cleanup(TrayNet *tn);

#endif /* ISDE_TRAY_NET_H */
