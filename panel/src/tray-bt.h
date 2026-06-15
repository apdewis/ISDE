/*
 * tray-bt.h — bluetooth tray module (BlueZ) for isde-panel
 */
#ifndef ISDE_TRAY_BT_H
#define ISDE_TRAY_BT_H

#include "panel.h"

#include <ISW/Toggle.h>
#include <ISW/ListBox.h>
#include <ISW/ListBoxRow.h>

#include <dbus/dbus.h>

/* ---------- D-Bus constants ---------- */

#define BLUEZ_SERVICE        "org.bluez"
#define BLUEZ_ADAPTER_IFACE  "org.bluez.Adapter1"
#define BLUEZ_DEVICE_IFACE   "org.bluez.Device1"
#define BLUEZ_AGENT_IFACE    "org.bluez.Agent1"
#define BLUEZ_AGENTMGR_IFACE "org.bluez.AgentManager1"
#define BLUEZ_PROPS_IFACE    "org.freedesktop.DBus.Properties"
#define BLUEZ_OBJMGR_IFACE   "org.freedesktop.DBus.ObjectManager"
#define BLUEZ_AGENT_PATH     "/org/isde/TrayBtAgent"

/* ---------- limits ---------- */

#define TB_MAX_DEVICES  64
#define TB_NAME_LEN    256
#define TB_PATH_LEN    512

/* ---------- BlueZ state ---------- */

typedef struct AdapterInfo {
    char  path[TB_PATH_LEN];
    char  name[TB_NAME_LEN];
    char  address[32];
    int   powered;
    int   discovering;
} AdapterInfo;

typedef struct BtDeviceInfo {
    char     path[TB_PATH_LEN];
    char     adapter[TB_PATH_LEN];
    char     name[TB_NAME_LEN];
    char     address[32];
    char     icon[64];
    int      paired;
    int      connected;
    int      trusted;
    int16_t  rssi;
    int      busy;
} BtDeviceInfo;

/* ---------- Icon states ---------- */

enum {
    TB_ICON_DISABLED = 0,
    TB_ICON_IDLE,
    TB_ICON_CONNECTED,
    TB_ICON_COUNT
};

/* ---------- Tray module state ---------- */

typedef struct TrayBt {
    struct Panel   *panel;

    Widget          icon;
    int             icon_state;

    /* Popup */
    Widget          popup_shell;
    Widget          popup_outer;
    Widget          popup_viewport;
    Widget          popup_listbox;
    int             popup_visible;

    /* BlueZ state */
    AdapterInfo     adapter;
    int             has_adapter;
    BtDeviceInfo      devices[TB_MAX_DEVICES];
    int             ndevices;

    /* D-Bus (system bus — BlueZ lives here) */
    DBusConnection *system_bus;

    /* Agent */
    DBusMessage    *pending_agent_req;
    Widget          agent_dialog;

    int             bluez_available;
} TrayBt;

/* ---------- tray-bt.c ---------- */
void tn_bt_init(struct Panel *p);
void tn_bt_cleanup(struct Panel *p);
void tn_bt_update_icon(TrayBt *tb);
void tn_bt_reload_theme(TrayBt *tb);

/* ---------- tray-bt-bluez.c ---------- */
int  tb_bluez_init(TrayBt *tb);
void tb_bluez_cleanup(TrayBt *tb);
int  tb_bluez_refresh(TrayBt *tb);
int  tb_bluez_get_adapter(TrayBt *tb);
int  tb_bluez_get_devices(TrayBt *tb);
int  tb_bluez_set_powered(TrayBt *tb, int powered);
int  tb_bluez_start_discovery(TrayBt *tb);
int  tb_bluez_stop_discovery(TrayBt *tb);
int  tb_bluez_device_connect(TrayBt *tb, const char *path);
int  tb_bluez_device_disconnect(TrayBt *tb, const char *path);
int  tb_bluez_device_pair(TrayBt *tb, const char *path);
int  tb_bluez_device_trust(TrayBt *tb, const char *path);
int  tb_bluez_device_remove(TrayBt *tb, const char *path);
BtDeviceInfo *tb_bluez_find_device(TrayBt *tb, const char *path);

/* ---------- tray-bt-agent.c ---------- */
int  tb_agent_init(TrayBt *tb);
void tb_agent_cleanup(TrayBt *tb);

/* ---------- tray-bt-menu.c ---------- */
void tb_menu_init(TrayBt *tb);
void tb_menu_show(TrayBt *tb);
void tb_menu_hide(TrayBt *tb);
void tb_menu_rebuild(TrayBt *tb);
void tb_menu_cleanup(TrayBt *tb);

#endif /* ISDE_TRAY_BT_H */
