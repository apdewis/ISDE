#define _POSIX_C_SOURCE 200809L
/*
 * pipewire.c — PipeWire integration for isde-tray-audio
 *
 * Connects to PipeWire, enumerates sinks and streams, tracks the
 * default sink via metadata, and provides volume/mute control.
 *
 * Sink volume is controlled via the Device Route param (the same
 * mechanism used by wpctl / pavucontrol).  Stream volume is
 * controlled via pw_node_set_param on the stream's node proxy.
 */
#include "tray-audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <pipewire/device.h>
#include <spa/utils/result.h>
#include <spa/utils/json.h>

/* Forward declarations */
static const struct pw_node_events sink_node_events;
static const struct pw_node_events stream_node_events;
static const struct pw_device_events device_events;

/* ---------- helpers ---------- */

SinkInfo *ta_find_sink(TrayAudio *ta, uint32_t id)
{
    for (int i = 0; i < ta->nsinks; i++) {
        if (ta->sinks[i].id == id)
            return &ta->sinks[i];
    }
    return NULL;
}

StreamInfo *ta_find_stream(TrayAudio *ta, uint32_t id)
{
    for (int i = 0; i < ta->nstreams; i++) {
        if (ta->streams[i].id == id)
            return &ta->streams[i];
    }
    return NULL;
}

SinkInfo *ta_default_sink(TrayAudio *ta)
{
    for (int i = 0; i < ta->nsinks; i++) {
        if (ta->sinks[i].is_default)
            return &ta->sinks[i];
    }
    if (ta->nsinks > 0)
        return &ta->sinks[0];
    return NULL;
}

DeviceInfo *ta_find_device(TrayAudio *ta, uint32_t id)
{
    for (int i = 0; i < ta->ndevices; i++) {
        if (ta->devices[i].id == id)
            return &ta->devices[i];
    }
    return NULL;
}

/* Find sink by its device_id + card_profile_device.
 * If card_profile_device hasn't been resolved yet (still 0), match
 * any sink on that device — the Route event will fill it in. */
static SinkInfo *find_sink_by_route(TrayAudio *ta, uint32_t device_id,
                                    int route_device)
{
    SinkInfo *fallback = NULL;
    for (int i = 0; i < ta->nsinks; i++) {
        if (ta->sinks[i].device_id != device_id)
            continue;
        if (ta->sinks[i].card_profile_device == route_device)
            return &ta->sinks[i];
        /* Unresolved sink on this device — candidate for assignment */
        if (ta->sinks[i].card_profile_device == 0 && !fallback)
            fallback = &ta->sinks[i];
    }
    if (fallback)
        fallback->card_profile_device = route_device;
    return fallback;
}

static void remove_sink(TrayAudio *ta, uint32_t id)
{
    for (int i = 0; i < ta->nsinks; i++) {
        if (ta->sinks[i].id == id) {
            if (ta->sinks[i].proxy) {
                spa_hook_remove(&ta->sinks[i].listener);
                pw_proxy_destroy(ta->sinks[i].proxy);
            }
            ta->nsinks--;
            if (i < ta->nsinks) {
                SinkInfo *moved = &ta->sinks[ta->nsinks];
                if (moved->proxy)
                    spa_hook_remove(&moved->listener);
                ta->sinks[i] = *moved;
                if (ta->sinks[i].proxy) {
                    pw_node_add_listener(
                        (struct pw_node *)ta->sinks[i].proxy,
                        &ta->sinks[i].listener,
                        &sink_node_events, &ta->sinks[i]);
                }
            }
            return;
        }
    }
}

static void remove_stream(TrayAudio *ta, uint32_t id)
{
    for (int i = 0; i < ta->nstreams; i++) {
        if (ta->streams[i].id == id) {
            if (ta->streams[i].proxy) {
                spa_hook_remove(&ta->streams[i].listener);
                pw_proxy_destroy(ta->streams[i].proxy);
            }
            ta->nstreams--;
            if (i < ta->nstreams) {
                StreamInfo *moved = &ta->streams[ta->nstreams];
                if (moved->proxy)
                    spa_hook_remove(&moved->listener);
                ta->streams[i] = *moved;
                if (ta->streams[i].proxy) {
                    pw_node_add_listener(
                        (struct pw_node *)ta->streams[i].proxy,
                        &ta->streams[i].listener,
                        &stream_node_events, &ta->streams[i]);
                }
            }
            return;
        }
    }
}

static void remove_device(TrayAudio *ta, uint32_t id)
{
    for (int i = 0; i < ta->ndevices; i++) {
        if (ta->devices[i].id == id) {
            if (ta->devices[i].proxy) {
                spa_hook_remove(&ta->devices[i].listener);
                pw_proxy_destroy(ta->devices[i].proxy);
            }
            ta->ndevices--;
            if (i < ta->ndevices) {
                DeviceInfo *moved = &ta->devices[ta->ndevices];
                if (moved->proxy)
                    spa_hook_remove(&moved->listener);
                ta->devices[i] = *moved;
                if (ta->devices[i].proxy) {
                    pw_device_add_listener(
                        (struct pw_device *)ta->devices[i].proxy,
                        &ta->devices[i].listener,
                        &device_events, &ta->devices[i]);
                }
            }
            return;
        }
    }
}

static float average_volume(const float *vols, int n)
{
    if (n <= 0)
        return 0.0f;
    float sum = 0.0f;
    for (int i = 0; i < n; i++)
        sum += vols[i];
    return sum / (float)n;
}

/* PipeWire channelVolumes are linear amplitude (cubic scale).
 * UI tools (wpctl, pavucontrol) convert with cube/cbrt so that
 * the slider feels perceptually linear.  We do the same. */
static float linear_to_cubic(float linear)
{
    return linear * linear * linear;
}

static float cubic_to_linear(float cubic)
{
    return cbrtf(cubic);
}

/* ---------- parse Props from a pod (used for streams and Route props) ---------- */

static void parse_props(const struct spa_pod *param,
                        float *channel_volumes, int *n_channels,
                        float *volume, int *muted)
{
    const struct spa_pod_prop *prop;
    struct spa_pod_object *obj = (struct spa_pod_object *)param;

    SPA_POD_OBJECT_FOREACH(obj, prop) {
        switch (prop->key) {
        case SPA_PROP_channelVolumes: {
            uint32_t n_vals;
            float *vals;
            n_vals = SPA_POD_ARRAY_N_VALUES(&prop->value);
            vals = SPA_POD_ARRAY_VALUES(&prop->value);
            if (n_vals > (uint32_t)MAX_CHANNELS)
                n_vals = MAX_CHANNELS;
            *n_channels = (int)n_vals;
            for (uint32_t i = 0; i < n_vals; i++)
                channel_volumes[i] = vals[i];
            *volume = cubic_to_linear(average_volume(channel_volumes, *n_channels));
            break;
        }
        case SPA_PROP_mute:
            *muted = !!SPA_POD_VALUE(struct spa_pod_bool, &prop->value);
            break;
        default:
            break;
        }
    }
}

/* ---------- device param events (Route) ---------- */

/* Device proxy user data layout: [TrayAudio*, uint32_t device_global_id] */
#define DEVICE_UDATA_SIZE (sizeof(TrayAudio *) + sizeof(uint32_t))

static void device_param_event(void *data, int seq,
                               uint32_t id, uint32_t index, uint32_t next,
                               const struct spa_pod *param)
{
    (void)seq; (void)index; (void)next;
    DeviceInfo *dev = data;
    TrayAudio *ta = dev->ta;

    fprintf(stderr, "isde-tray-audio: device_param_event dev=%u param_id=%u param=%p\n",
            dev->id, id, (void *)param);

    if (id != SPA_PARAM_Route || param == NULL)
        return;

    int route_index = -1;
    int route_device = -1;
    int direction = -1;
    const struct spa_pod *props_pod = NULL;

    const struct spa_pod_prop *prop;
    struct spa_pod_object *obj = (struct spa_pod_object *)param;

    SPA_POD_OBJECT_FOREACH(obj, prop) {
        switch (prop->key) {
        case SPA_PARAM_ROUTE_index:
            route_index = SPA_POD_VALUE(struct spa_pod_int, &prop->value);
            break;
        case SPA_PARAM_ROUTE_device:
            route_device = SPA_POD_VALUE(struct spa_pod_int, &prop->value);
            break;
        case SPA_PARAM_ROUTE_direction:
            direction = SPA_POD_VALUE(struct spa_pod_id, &prop->value);
            break;
        case SPA_PARAM_ROUTE_props:
            props_pod = &prop->value;
            break;
        }
    }

    if (direction != SPA_DIRECTION_OUTPUT)
        return;
    if (route_device < 0 || !props_pod)
        return;

    SinkInfo *sink = find_sink_by_route(ta, dev->id, route_device);
    if (!sink) {
        fprintf(stderr, "isde-tray-audio: route event dev=%u route_dev=%d — no matching sink\n",
                dev->id, route_device);
        return;
    }

    sink->route_index = route_index;
    parse_props(props_pod, sink->channel_volumes, &sink->n_channels,
                &sink->volume, &sink->muted);
    fprintf(stderr, "isde-tray-audio: route event dev=%u sink=%u route_idx=%d vol=%.2f muted=%d\n",
            dev->id, sink->id, route_index, sink->volume, sink->muted);

    if (sink->is_default)
        tray_audio_update_icon(ta);
    if (ta->popup_visible)
        ta_popup_update(ta);
}

static const struct pw_device_events device_events = {
    PW_VERSION_DEVICE_EVENTS,
    .param = device_param_event,
};

/* ---------- node param events (for streams) ---------- */

static void sink_param_event(void *data, int seq,
                             uint32_t id, uint32_t index, uint32_t next,
                             const struct spa_pod *param)
{
    (void)seq; (void)index; (void)next; (void)data;

    /* Sink volume is now tracked via device Route params, not node Props.
     * We still subscribe to node Props for n_channels fallback, but
     * the authoritative volume comes from the device Route. */
    (void)id; (void)param;
}

static const struct pw_node_events sink_node_events = {
    PW_VERSION_NODE_EVENTS,
    .param = sink_param_event,
};

static void stream_param_event(void *data, int seq,
                               uint32_t id, uint32_t index, uint32_t next,
                               const struct spa_pod *param)
{
    (void)seq; (void)index; (void)next;
    StreamInfo *stream = data;

    if (id != SPA_PARAM_Props || param == NULL)
        return;

    parse_props(param, stream->channel_volumes, &stream->n_channels,
                &stream->volume, &stream->muted);
}

static const struct pw_node_events stream_node_events = {
    PW_VERSION_NODE_EVENTS,
    .param = stream_param_event,
};

/* ---------- metadata events ---------- */

static int metadata_property(void *data, uint32_t subject,
                             const char *key, const char *type,
                             const char *value)
{
    (void)type;
    TrayAudio *ta = data;

    if (subject != 0 || key == NULL)
        return 0;

    if (strcmp(key, "default.audio.sink") != 0)
        return 0;

    for (int i = 0; i < ta->nsinks; i++)
        ta->sinks[i].is_default = 0;

    if (value == NULL) {
        ta->default_sink_id = 0;
        tray_audio_update_icon(ta);
        return 0;
    }

    struct spa_json it[2];
    char name[256] = {0};

    spa_json_init(&it[0], value, strlen(value));
    if (spa_json_enter_object(&it[0], &it[1]) <= 0)
        return 0;

    char k[256];
    while (spa_json_get_string(&it[1], k, sizeof(k)) > 0) {
        if (strcmp(k, "name") == 0) {
            spa_json_get_string(&it[1], name, sizeof(name));
            break;
        } else {
            char skip[512];
            spa_json_get_string(&it[1], skip, sizeof(skip));
        }
    }

    if (name[0]) {
        for (int i = 0; i < ta->nsinks; i++) {
            if (strcmp(ta->sinks[i].node_name, name) == 0) {
                ta->sinks[i].is_default = 1;
                ta->default_sink_id = ta->sinks[i].id;
                break;
            }
        }
    }

    tray_audio_update_icon(ta);
    if (ta->popup_visible)
        ta_popup_update(ta);
    return 0;
}

static const struct pw_metadata_events metadata_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = metadata_property,
};

/* ---------- registry events ---------- */

static void bind_device(TrayAudio *ta, uint32_t id)
{
    if (ta_find_device(ta, id))
        return;
    if (ta->ndevices >= MAX_DEVICES)
        return;

    DeviceInfo *dev = &ta->devices[ta->ndevices];
    memset(dev, 0, sizeof(*dev));
    dev->id = id;
    dev->ta = ta;

    dev->proxy = pw_registry_bind(ta->pw_registry, id,
                                  PW_TYPE_INTERFACE_Device,
                                  PW_VERSION_DEVICE, 0);
    if (dev->proxy) {
        pw_device_add_listener((struct pw_device *)dev->proxy,
                               &dev->listener, &device_events, dev);

        uint32_t params[] = { SPA_PARAM_Route };
        pw_device_subscribe_params((struct pw_device *)dev->proxy, params, 1);
        pw_device_enum_params((struct pw_device *)dev->proxy,
                              0, SPA_PARAM_Route, 0, -1, NULL);
    }

    ta->ndevices++;
}

static void bind_sink(TrayAudio *ta, uint32_t id, const struct spa_dict *props)
{
    if (ta->nsinks >= MAX_SINKS)
        return;

    SinkInfo *sink = &ta->sinks[ta->nsinks];
    memset(sink, 0, sizeof(*sink));
    sink->id = id;
    sink->volume = 1.0f;
    sink->route_index = -1;

    const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    if (desc)
        snprintf(sink->name, sizeof(sink->name), "%s", desc);
    else
        snprintf(sink->name, sizeof(sink->name), "Sink %u", id);

    const char *nname = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (nname)
        snprintf(sink->node_name, sizeof(sink->node_name), "%s", nname);

    /* Track the device this sink belongs to */
    const char *dev_id_str = spa_dict_lookup(props, PW_KEY_DEVICE_ID);
    if (dev_id_str)
        sink->device_id = (uint32_t)atoi(dev_id_str);

    const char *cpd_str = spa_dict_lookup(props, "card.profile.device");
    if (cpd_str)
        sink->card_profile_device = atoi(cpd_str);

    fprintf(stderr, "isde-tray-audio: bind_sink id=%u device_id=%u card_profile_device=%d (from props: dev='%s' cpd='%s')\n",
            id, sink->device_id, sink->card_profile_device,
            dev_id_str ? dev_id_str : "(null)", cpd_str ? cpd_str : "(null)");

    /* Bind node proxy (still useful for stream-like operations) */
    sink->proxy = pw_registry_bind(ta->pw_registry, id,
                                   PW_TYPE_INTERFACE_Node,
                                   PW_VERSION_NODE, sizeof(TrayAudio *));
    if (sink->proxy) {
        *(TrayAudio **)pw_proxy_get_user_data(sink->proxy) = ta;
        pw_node_add_listener((struct pw_node *)sink->proxy,
                             &sink->listener, &sink_node_events, sink);
    }

    ta->nsinks++;

    /* Ensure the device is bound so we get Route params */
    if (sink->device_id)
        bind_device(ta, sink->device_id);
}

static void bind_stream(TrayAudio *ta, uint32_t id, const struct spa_dict *props)
{
    if (ta->nstreams >= MAX_STREAMS)
        return;

    StreamInfo *stream = &ta->streams[ta->nstreams];
    memset(stream, 0, sizeof(*stream));
    stream->id = id;
    stream->volume = 1.0f;

    const char *app = spa_dict_lookup(props, PW_KEY_APP_NAME);
    if (!app)
        app = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    if (!app)
        app = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (app)
        snprintf(stream->name, sizeof(stream->name), "%s", app);
    else
        snprintf(stream->name, sizeof(stream->name), "Stream %u", id);

    stream->proxy = pw_registry_bind(ta->pw_registry, id,
                                     PW_TYPE_INTERFACE_Node,
                                     PW_VERSION_NODE, sizeof(TrayAudio *));
    if (stream->proxy) {
        *(TrayAudio **)pw_proxy_get_user_data(stream->proxy) = ta;
        pw_node_add_listener((struct pw_node *)stream->proxy,
                             &stream->listener, &stream_node_events, stream);

        uint32_t params[] = { SPA_PARAM_Props };
        pw_node_subscribe_params((struct pw_node *)stream->proxy, params, 1);
        pw_node_enum_params((struct pw_node *)stream->proxy,
                            0, SPA_PARAM_Props, 0, -1, NULL);
    }

    ta->nstreams++;
}

static void registry_global(void *data, uint32_t id, uint32_t permissions,
                            const char *type, uint32_t version,
                            const struct spa_dict *props)
{
    (void)permissions; (void)version;
    TrayAudio *ta = data;

    if (props == NULL)
        return;

    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (!media_class)
            return;

        if (strcmp(media_class, "Audio/Sink") == 0) {
            bind_sink(ta, id, props);
        } else if (strcmp(media_class, "Stream/Output/Audio") == 0) {
            bind_stream(ta, id, props);
        }
    } else if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (name && strcmp(name, "default") == 0 && ta->metadata == NULL) {
            ta->metadata = pw_registry_bind(ta->pw_registry, id,
                                            PW_TYPE_INTERFACE_Metadata,
                                            PW_VERSION_METADATA, 0);
            if (ta->metadata) {
                pw_metadata_add_listener(
                    (struct pw_metadata *)ta->metadata,
                    &ta->metadata_listener,
                    &metadata_events, ta);
            }
        }
    }
}

static void registry_global_remove(void *data, uint32_t id)
{
    TrayAudio *ta = data;
    remove_sink(ta, id);
    remove_stream(ta, id);
    remove_device(ta, id);
    tray_audio_update_icon(ta);
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ---------- core events ---------- */

static void core_error(void *data, uint32_t id, int seq, int res,
                       const char *message)
{
    (void)id; (void)seq;
    TrayAudio *ta = data;

    fprintf(stderr, "isde-tray-audio: PipeWire error %d: %s\n", res, message);

    if (id == 0 && res == -EPIPE) {
        ta->running = 0;
    }
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .error = core_error,
};

/* ---------- Xt event loop integration ---------- */

static void pw_input_cb(IswPointer client_data, int *fd, IswInputId *input_id)
{
    (void)fd; (void)input_id;
    TrayAudio *ta = (TrayAudio *)client_data;

    if (!ta->pw_loop)
        return;

    pw_loop_enter(ta->pw_loop);
    pw_loop_iterate(ta->pw_loop, 0);
    pw_loop_leave(ta->pw_loop);
}

/* ---------- public API ---------- */

int ta_pw_init(TrayAudio *ta)
{
    pw_init(NULL, NULL);

    ta->pw_main_loop = pw_main_loop_new(NULL);
    if (!ta->pw_main_loop) {
        fprintf(stderr, "isde-tray-audio: pw_main_loop_new failed\n");
        return -1;
    }
    ta->pw_loop = pw_main_loop_get_loop(ta->pw_main_loop);

    ta->pw_context = pw_context_new(ta->pw_loop, NULL, 0);
    if (!ta->pw_context) {
        fprintf(stderr, "isde-tray-audio: pw_context_new failed\n");
        pw_main_loop_destroy(ta->pw_main_loop);
        ta->pw_main_loop = NULL;
        ta->pw_loop = NULL;
        return -1;
    }

    ta->pw_core = pw_context_connect(ta->pw_context, NULL, 0);
    if (!ta->pw_core) {
        fprintf(stderr, "isde-tray-audio: pw_context_connect failed\n");
        pw_context_destroy(ta->pw_context);
        ta->pw_context = NULL;
        pw_main_loop_destroy(ta->pw_main_loop);
        ta->pw_main_loop = NULL;
        ta->pw_loop = NULL;
        return -1;
    }

    pw_core_add_listener(ta->pw_core, &ta->core_listener,
                         &core_events, ta);

    ta->pw_registry = pw_core_get_registry(ta->pw_core,
                                           PW_VERSION_REGISTRY, 0);
    if (!ta->pw_registry) {
        fprintf(stderr, "isde-tray-audio: pw_core_get_registry failed\n");
        pw_core_disconnect(ta->pw_core);
        ta->pw_core = NULL;
        pw_context_destroy(ta->pw_context);
        ta->pw_context = NULL;
        pw_main_loop_destroy(ta->pw_main_loop);
        ta->pw_main_loop = NULL;
        ta->pw_loop = NULL;
        return -1;
    }

    pw_registry_add_listener(ta->pw_registry, &ta->registry_listener,
                             &registry_events, ta);

    int fd = pw_loop_get_fd(ta->pw_loop);
    if (fd >= 0) {
        IswAppAddInput(ta->app, fd, (IswPointer)IswInputReadMask,
                       pw_input_cb, ta);
    }

    pw_core_sync(ta->pw_core, 0, 0);

    return 0;
}

void ta_pw_cleanup(TrayAudio *ta)
{
    for (int i = 0; i < ta->nsinks; i++) {
        if (ta->sinks[i].proxy) {
            spa_hook_remove(&ta->sinks[i].listener);
            pw_proxy_destroy(ta->sinks[i].proxy);
            ta->sinks[i].proxy = NULL;
        }
    }
    for (int i = 0; i < ta->nstreams; i++) {
        if (ta->streams[i].proxy) {
            spa_hook_remove(&ta->streams[i].listener);
            pw_proxy_destroy(ta->streams[i].proxy);
            ta->streams[i].proxy = NULL;
        }
    }
    for (int i = 0; i < ta->ndevices; i++) {
        if (ta->devices[i].proxy) {
            spa_hook_remove(&ta->devices[i].listener);
            pw_proxy_destroy(ta->devices[i].proxy);
            ta->devices[i].proxy = NULL;
        }
    }

    if (ta->metadata) {
        spa_hook_remove(&ta->metadata_listener);
        pw_proxy_destroy(ta->metadata);
        ta->metadata = NULL;
    }

    if (ta->pw_registry) {
        spa_hook_remove(&ta->registry_listener);
        pw_proxy_destroy((struct pw_proxy *)ta->pw_registry);
        ta->pw_registry = NULL;
    }

    if (ta->pw_core) {
        spa_hook_remove(&ta->core_listener);
        pw_core_disconnect(ta->pw_core);
        ta->pw_core = NULL;
    }

    if (ta->pw_context) {
        pw_context_destroy(ta->pw_context);
        ta->pw_context = NULL;
    }

    if (ta->pw_loop) {
        pw_main_loop_destroy(ta->pw_main_loop);
        ta->pw_main_loop = NULL;
        ta->pw_loop = NULL;
    }

    pw_deinit();
}

/* ---------- volume / mute control ---------- */

/* Set volume on a sink via its Device's Route param */
static void set_sink_volume(TrayAudio *ta, SinkInfo *sink, float volume)
{
    DeviceInfo *dev = ta_find_device(ta, sink->device_id);
    if (!dev || !dev->proxy || sink->route_index < 0) {
        fprintf(stderr, "isde-tray-audio: set_sink_volume BAIL dev=%p proxy=%p route_idx=%d device_id=%u\n",
                (void *)dev, dev ? (void *)dev->proxy : NULL, sink->route_index, sink->device_id);
        return;
    }
    fprintf(stderr, "isde-tray-audio: set_sink_volume sink=%u vol=%.2f route_idx=%d card_dev=%d\n",
            sink->id, volume, sink->route_index, sink->card_profile_device);

    float cubic = linear_to_cubic(volume);
    int nc = sink->n_channels > 0 ? sink->n_channels : 2;
    float vols[MAX_CHANNELS];
    for (int i = 0; i < nc; i++)
        vols[i] = cubic;

    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    struct spa_pod_frame f[2];

    spa_pod_builder_push_object(&b, &f[0],
                                SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
    spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_index, 0);
    spa_pod_builder_int(&b, sink->route_index);
    spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_device, 0);
    spa_pod_builder_int(&b, sink->card_profile_device);
    spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_props, 0);

    spa_pod_builder_push_object(&b, &f[1],
                                SPA_TYPE_OBJECT_Props, SPA_PARAM_Route);
    spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
    spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float, nc, vols);
    spa_pod_builder_pop(&b, &f[1]);

    struct spa_pod *pod = spa_pod_builder_pop(&b, &f[0]);

    pw_device_set_param((struct pw_device *)dev->proxy,
                        SPA_PARAM_Route, 0, pod);
}

/* Set mute on a sink via its Device's Route param */
static void set_sink_mute(TrayAudio *ta, SinkInfo *sink, int muted)
{
    DeviceInfo *dev = ta_find_device(ta, sink->device_id);
    if (!dev || !dev->proxy || sink->route_index < 0)
        return;

    uint8_t buf[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    struct spa_pod_frame f[2];

    spa_pod_builder_push_object(&b, &f[0],
                                SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
    spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_index, 0);
    spa_pod_builder_int(&b, sink->route_index);
    spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_device, 0);
    spa_pod_builder_int(&b, sink->card_profile_device);
    spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_props, 0);

    spa_pod_builder_push_object(&b, &f[1],
                                SPA_TYPE_OBJECT_Props, SPA_PARAM_Route);
    spa_pod_builder_prop(&b, SPA_PROP_mute, 0);
    spa_pod_builder_bool(&b, muted ? true : false);
    spa_pod_builder_pop(&b, &f[1]);

    struct spa_pod *pod = spa_pod_builder_pop(&b, &f[0]);

    pw_device_set_param((struct pw_device *)dev->proxy,
                        SPA_PARAM_Route, 0, pod);
}

/* Set volume on a stream via pw_node_set_param (streams accept this) */
static void set_stream_volume(struct pw_proxy *proxy, float volume, int n_channels)
{
    if (!proxy || n_channels <= 0)
        return;

    float cubic = linear_to_cubic(volume);
    float vols[MAX_CHANNELS];
    for (int i = 0; i < n_channels; i++)
        vols[i] = cubic;

    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

    struct spa_pod_frame f;
    spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
    spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float,
                          n_channels, vols);
    struct spa_pod *pod = spa_pod_builder_pop(&b, &f);

    pw_node_set_param((struct pw_node *)proxy,
                      SPA_PARAM_Props, 0, pod);
}

static void set_stream_mute(struct pw_proxy *proxy, int muted)
{
    if (!proxy)
        return;

    uint8_t buf[256];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

    struct spa_pod_frame f;
    spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(&b, SPA_PROP_mute, 0);
    spa_pod_builder_bool(&b, muted ? true : false);
    struct spa_pod *pod = spa_pod_builder_pop(&b, &f);

    pw_node_set_param((struct pw_node *)proxy,
                      SPA_PARAM_Props, 0, pod);
}

void ta_pw_set_volume(TrayAudio *ta, uint32_t node_id, float volume)
{
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;

    SinkInfo *sink = ta_find_sink(ta, node_id);
    if (sink) {
        set_sink_volume(ta, sink, volume);
        return;
    }

    StreamInfo *stream = ta_find_stream(ta, node_id);
    if (stream) {
        int nc = stream->n_channels > 0 ? stream->n_channels : 2;
        set_stream_volume(stream->proxy, volume, nc);
    }
}

void ta_pw_set_mute(TrayAudio *ta, uint32_t node_id, int muted)
{
    SinkInfo *sink = ta_find_sink(ta, node_id);
    if (sink) {
        set_sink_mute(ta, sink, muted);
        return;
    }

    StreamInfo *stream = ta_find_stream(ta, node_id);
    if (stream)
        set_stream_mute(stream->proxy, muted);
}

void ta_pw_set_default_sink(TrayAudio *ta, uint32_t node_id)
{
    SinkInfo *sink = ta_find_sink(ta, node_id);
    if (!sink || !ta->metadata)
        return;

    char json[512];
    snprintf(json, sizeof(json), "{\"name\":\"%s\"}", sink->node_name);

    pw_metadata_set_property((struct pw_metadata *)ta->metadata,
                             0, "default.audio.sink",
                             "Spa:String:JSON", json);
}
