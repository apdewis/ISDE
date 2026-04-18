/*
 * tray-audio.h — isde-tray-audio internal header
 */
#ifndef ISDE_TRAY_AUDIO_H
#define ISDE_TRAY_AUDIO_H

#include <ISW/Intrinsic.h>
#include <ISW/StringDefs.h>
#include <ISW/Shell.h>
#include <ISW/SimpleMenu.h>
#include <ISW/SmeBSB.h>
#include <ISW/SmeLine.h>
#include <ISW/Tabs.h>
#include <ISW/Slider.h>
#include <ISW/Command.h>
#include <ISW/Toggle.h>
#include <ISW/Label.h>
#include <ISW/Form.h>
#include <ISW/Box.h>
#include <ISW/IswTrayIcon.h>
#include <ISW/ISWSVG.h>

#include <xcb/xcb.h>

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/props.h>
#include <spa/param/route.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <spa/pod/iter.h>
#include <spa/param/param.h>

#include "isde/isde-dbus.h"
#include "isde/isde-theme.h"
#include "isde/isde-config.h"
#include "isde/isde-xdg.h"

/* ---------- limits ---------- */

#define MAX_SINKS    32
#define MAX_STREAMS  64
#define MAX_CHANNELS 16
#define MAX_DEVICES  16

/* ---------- audio state ---------- */

typedef struct DeviceInfo {
    uint32_t         id;                    /* PipeWire device global ID */
    struct pw_proxy *proxy;
    struct spa_hook  listener;
    void            *ta;                    /* TrayAudio back-pointer */
} DeviceInfo;

typedef struct SinkInfo {
    uint32_t    id;                         /* PipeWire node ID */
    char        name[256];                  /* node.description */
    char        node_name[256];             /* node.name (for metadata matching) */
    float       channel_volumes[MAX_CHANNELS];
    int         n_channels;
    float       volume;                     /* 0.0–1.0 average */
    int         muted;
    int         is_default;
    uint32_t    device_id;                  /* PW device global ID (from device.id prop) */
    int         card_profile_device;        /* device-internal index (from card.profile.device) */
    int         route_index;                /* active route index for this sink */
    struct pw_proxy *proxy;
    struct spa_hook  listener;
} SinkInfo;

typedef struct StreamInfo {
    uint32_t    id;                         /* PipeWire node ID */
    char        name[256];                  /* application.name */
    float       channel_volumes[MAX_CHANNELS];
    int         n_channels;
    float       volume;                     /* 0.0–1.0 average */
    int         muted;
    struct pw_proxy *proxy;
    struct spa_hook  listener;
} StreamInfo;

/* ---------- applet state ---------- */

typedef struct TrayAudio {
    IswAppContext        app;
    Widget               toplevel;

    /* Tray icon */
    IswTrayIcon          tray_icon;
    int                  icon_state;        /* 0=muted 1=low 2=med 3=high */

    /* Popup */
    Widget               popup_shell;
    Widget               tabs;
    Widget               output_page;
    Widget               app_page;
    int                  popup_visible;

    /* Right-click menu */
    Widget               menu_shell;

    /* Audio state */
    SinkInfo             sinks[MAX_SINKS];
    int                  nsinks;
    uint32_t             default_sink_id;   /* PW node ID from metadata */

    StreamInfo           streams[MAX_STREAMS];
    int                  nstreams;

    DeviceInfo           devices[MAX_DEVICES];
    int                  ndevices;

    /* PipeWire */
    struct pw_main_loop *pw_main_loop;
    struct pw_loop      *pw_loop;       /* from pw_main_loop_get_loop */
    struct pw_context   *pw_context;
    struct pw_core      *pw_core;
    struct pw_registry  *pw_registry;
    struct spa_hook      registry_listener;
    struct spa_hook      core_listener;

    /* Metadata proxy for default sink tracking */
    struct pw_proxy     *metadata;
    struct spa_hook      metadata_listener;

    /* Session D-Bus (theme changes) */
    IsdeDBus            *session_dbus;

    int                  running;
    int                  restart;
} TrayAudio;

/* ---------- tray-audio.c ---------- */
int  tray_audio_init(TrayAudio *ta, int *argc, char **argv);
void tray_audio_run(TrayAudio *ta);
void tray_audio_cleanup(TrayAudio *ta);
void tray_audio_update_icon(TrayAudio *ta);

/* ---------- pipewire.c ---------- */
int  ta_pw_init(TrayAudio *ta);
void ta_pw_cleanup(TrayAudio *ta);
void ta_pw_set_volume(TrayAudio *ta, uint32_t node_id, float volume);
void ta_pw_set_mute(TrayAudio *ta, uint32_t node_id, int muted);
void ta_pw_set_default_sink(TrayAudio *ta, uint32_t node_id);

/* Lookup helpers */
SinkInfo   *ta_find_sink(TrayAudio *ta, uint32_t id);
StreamInfo *ta_find_stream(TrayAudio *ta, uint32_t id);
SinkInfo   *ta_default_sink(TrayAudio *ta);
DeviceInfo *ta_find_device(TrayAudio *ta, uint32_t id);

/* ---------- popup.c ---------- */
void ta_popup_init(TrayAudio *ta);
void ta_popup_show(TrayAudio *ta);
void ta_popup_hide(TrayAudio *ta);
void ta_popup_update(TrayAudio *ta);
void ta_popup_cleanup(TrayAudio *ta);

void ta_menu_init(TrayAudio *ta);
void ta_menu_show(TrayAudio *ta);
void ta_menu_cleanup(TrayAudio *ta);

#endif /* ISDE_TRAY_AUDIO_H */
