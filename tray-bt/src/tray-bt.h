/*
 * tray-bt.h — isde-tray-bt internal header
 */
#ifndef ISDE_TRAY_BT_H
#define ISDE_TRAY_BT_H

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

#define BLUEZ_SERVICE        "org.bluez"
#define BLUEZ_ADAPTER_IFACE  "org.bluez.Adapter1"
#define BLUEZ_DEVICE_IFACE   "org.bluez.Device1"
#define BLUEZ_AGENT_IFACE    "org.bluez.Agent1"
#define BLUEZ_AGENTMGR_IFACE "org.bluez.AgentManager1"
#define BLUEZ_PROPS_IFACE    "org.freedesktop.DBus.Properties"
#define BLUEZ_OBJMGR_IFACE   "org.freedesktop.DBus.ObjectManager"
#define BLUEZ_AGENT_PATH     "/org/isde/TrayBtAgent"

/* ---------- limits ---------- */

#define MAX_DEVICES  64
#define NAME_LEN    256
#define PATH_LEN    512

/* ---------- BlueZ state ---------- */

typedef struct AdapterInfo {
    char  path[PATH_LEN];
    char  name[NAME_LEN];
    char  address[32];
    int   powered;
    int   discovering;
} AdapterInfo;

typedef struct DeviceInfo {
    char     path[PATH_LEN];
    char     adapter[PATH_LEN];
    char     name[NAME_LEN];
    char     address[32];
    char     icon[64];
    int      paired;
    int      connected;
    int      trusted;
    int16_t  rssi;
} DeviceInfo;

/* ---------- Icon states ---------- */

enum {
    ICON_BT_DISABLED = 0,
    ICON_BT_IDLE,
    ICON_BT_CONNECTED,
    ICON_BT_COUNT
};

/* ---------- Tray applet state ---------- */

typedef struct TrayBt {
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

    /* BlueZ state */
    AdapterInfo         adapter;
    int                 has_adapter;
    DeviceInfo          devices[MAX_DEVICES];
    int                 ndevices;

    /* D-Bus */
    DBusConnection     *system_bus;
    IsdeDBus           *session_dbus;

    /* Agent */
    DBusMessage        *pending_agent_req;
    Widget              agent_dialog;

    int                 bluez_available;
    int                 running;
    int                 restart;
} TrayBt;

/* ---------- tray-bt.c ---------- */
int  tray_bt_init(TrayBt *tb, int *argc, char **argv);
void tray_bt_run(TrayBt *tb);
void tray_bt_cleanup(TrayBt *tb);
void tray_bt_update_icon(TrayBt *tb);

/* ---------- bluez.c ---------- */
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

/* ---------- agent.c ---------- */
int  tb_agent_init(TrayBt *tb);
void tb_agent_cleanup(TrayBt *tb);

/* ---------- menu.c ---------- */
void tb_menu_init(TrayBt *tb);
void tb_menu_show(TrayBt *tb);
void tb_menu_hide(TrayBt *tb);
void tb_menu_rebuild(TrayBt *tb);
void tb_menu_cleanup(TrayBt *tb);

#endif /* ISDE_TRAY_BT_H */
