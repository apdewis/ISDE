#define _POSIX_C_SOURCE 200809L
/*
 * pipewire.c — PipeWire integration for isde-tray-audio
 *
 * Connects to PipeWire, enumerates sinks and streams, tracks the
 * default sink via metadata, and provides volume/mute control.
 */
#include "tray-audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <spa/utils/result.h>
#include <spa/utils/json.h>

/* Forward declarations for node event structs */
static const struct pw_node_events sink_node_events;
static const struct pw_node_events stream_node_events;

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
    /* Fall back to first sink */
    if (ta->nsinks > 0)
        return &ta->sinks[0];
    return NULL;
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
                /* Move last entry into this slot.  Must re-register the
                 * listener so the callback data pointer is updated. */
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

static float average_volume(const float *vols, int n)
{
    if (n <= 0)
        return 0.0f;
    float sum = 0.0f;
    for (int i = 0; i < n; i++)
        sum += vols[i];
    return sum / (float)n;
}

/* ---------- node param events ---------- */

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
            *volume = average_volume(channel_volumes, *n_channels);
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

static void sink_param_event(void *data, int seq,
                             uint32_t id, uint32_t index, uint32_t next,
                             const struct spa_pod *param)
{
    (void)seq; (void)index; (void)next;
    SinkInfo *sink = data;

    if (id != SPA_PARAM_Props || param == NULL)
        return;

    parse_props(param, sink->channel_volumes, &sink->n_channels,
                &sink->volume, &sink->muted);

    /* Find the TrayAudio from the proxy user data.
     * We store TrayAudio* in the proxy user data area. */
    TrayAudio *ta = *(TrayAudio **)pw_proxy_get_user_data(sink->proxy);
    if (sink->is_default)
        tray_audio_update_icon(ta);
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

    /* Clear old default */
    for (int i = 0; i < ta->nsinks; i++)
        ta->sinks[i].is_default = 0;

    if (value == NULL) {
        ta->default_sink_id = 0;
        tray_audio_update_icon(ta);
        return 0;
    }

    /* Value is JSON: {"name":"sink_name"} — parse the name field */
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
            /* Skip value */
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

static void bind_sink(TrayAudio *ta, uint32_t id, const struct spa_dict *props)
{
    if (ta->nsinks >= MAX_SINKS)
        return;

    SinkInfo *sink = &ta->sinks[ta->nsinks];
    memset(sink, 0, sizeof(*sink));
    sink->id = id;
    sink->volume = 1.0f;

    const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    if (desc)
        snprintf(sink->name, sizeof(sink->name), "%s", desc);
    else
        snprintf(sink->name, sizeof(sink->name), "Sink %u", id);

    const char *nname = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (nname)
        snprintf(sink->node_name, sizeof(sink->node_name), "%s", nname);

    /* Bind a proxy to subscribe to param changes */
    sink->proxy = pw_registry_bind(ta->pw_registry, id,
                                   PW_TYPE_INTERFACE_Node,
                                   PW_VERSION_NODE, sizeof(TrayAudio *));
    if (sink->proxy) {
        *(TrayAudio **)pw_proxy_get_user_data(sink->proxy) = ta;
        pw_node_add_listener((struct pw_node *)sink->proxy,
                             &sink->listener, &sink_node_events, sink);

        uint32_t params[] = { SPA_PARAM_Props };
        pw_node_subscribe_params((struct pw_node *)sink->proxy, params, 1);
        pw_node_enum_params((struct pw_node *)sink->proxy,
                            0, SPA_PARAM_Props, 0, -1, NULL);
    }

    ta->nsinks++;
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
        /* Lost connection */
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

    /* Process any pending PipeWire events */
    pw_loop_enter(ta->pw_loop);
    pw_loop_iterate(ta->pw_loop, 0);
    pw_loop_leave(ta->pw_loop);
}

/* Flush outgoing messages.  pw_node_set_param and pw_core_sync both
 * buffer messages internally — a loop iteration is needed to actually
 * write them to the socket.  This single iteration flushes the write
 * buffer; the response will arrive later and be handled by pw_input_cb. */
static int pw_flushing = 0;

static void ta_pw_flush(TrayAudio *ta)
{
    if (!ta->pw_loop || pw_flushing)
        return;

    pw_flushing = 1;
    pw_loop_enter(ta->pw_loop);
    pw_loop_iterate(ta->pw_loop, 0);
    pw_loop_leave(ta->pw_loop);
    pw_flushing = 0;
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

    /* Register PipeWire fd with the Xt event loop */
    int fd = pw_loop_get_fd(ta->pw_loop);
    if (fd >= 0) {
        IswAppAddInput(ta->app, fd, (IswPointer)IswInputReadMask,
                       pw_input_cb, ta);
    }

    /* Initial sync to start populating sinks/streams.
     * Responses arrive via pw_input_cb as the event loop runs. */
    pw_core_sync(ta->pw_core, 0, 0);

    return 0;
}

void ta_pw_cleanup(TrayAudio *ta)
{
    /* Destroy node proxies */
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

static void set_node_volume(struct pw_proxy *proxy, float volume, int n_channels)
{
    if (!proxy || n_channels <= 0)
        return;

    float vols[MAX_CHANNELS];
    for (int i = 0; i < n_channels; i++)
        vols[i] = volume;

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

static void set_node_mute(struct pw_proxy *proxy, int muted)
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
        int nc = sink->n_channels > 0 ? sink->n_channels : 2;
        fprintf(stderr, "isde-tray-audio: set_volume sink id=%u vol=%.2f nc=%d proxy=%p\n",
                node_id, volume, nc, (void *)sink->proxy);
        set_node_volume(sink->proxy, volume, nc);
    } else {
        StreamInfo *stream = ta_find_stream(ta, node_id);
        if (stream) {
            int nc = stream->n_channels > 0 ? stream->n_channels : 2;
            fprintf(stderr, "isde-tray-audio: set_volume stream id=%u vol=%.2f nc=%d proxy=%p\n",
                    node_id, volume, nc, (void *)stream->proxy);
            set_node_volume(stream->proxy, volume, nc);
        } else {
            fprintf(stderr, "isde-tray-audio: set_volume id=%u NOT FOUND (nsinks=%d nstreams=%d)\n",
                    node_id, ta->nsinks, ta->nstreams);
        }
    }

    ta_pw_flush(ta);
}

void ta_pw_set_mute(TrayAudio *ta, uint32_t node_id, int muted)
{
    SinkInfo *sink = ta_find_sink(ta, node_id);
    if (sink) {
        fprintf(stderr, "isde-tray-audio: set_mute sink id=%u muted=%d proxy=%p\n",
                node_id, muted, (void *)sink->proxy);
        set_node_mute(sink->proxy, muted);
    } else {
        StreamInfo *stream = ta_find_stream(ta, node_id);
        if (stream) {
            fprintf(stderr, "isde-tray-audio: set_mute stream id=%u muted=%d proxy=%p\n",
                    node_id, muted, (void *)stream->proxy);
            set_node_mute(stream->proxy, muted);
        } else {
            fprintf(stderr, "isde-tray-audio: set_mute id=%u NOT FOUND\n", node_id);
        }
    }

    ta_pw_flush(ta);
}

void ta_pw_set_default_sink(TrayAudio *ta, uint32_t node_id)
{
    SinkInfo *sink = ta_find_sink(ta, node_id);
    if (!sink || !ta->metadata)
        return;

    /* Build JSON value: {"name":"<node_name>"} */
    char json[512];
    snprintf(json, sizeof(json), "{\"name\":\"%s\"}", sink->node_name);

    pw_metadata_set_property((struct pw_metadata *)ta->metadata,
                             0, "default.audio.sink",
                             "Spa:String:JSON", json);

    ta_pw_flush(ta);
}
