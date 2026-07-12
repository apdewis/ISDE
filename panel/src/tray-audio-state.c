#define _POSIX_C_SOURCE 200809L
/*
 * tray-audio-state.c — persistence & reapplication of manual audio choices
 *
 * Tracks the user's most recent manual choices (default sink/source and
 * per-node volume/mute for nodes the user actually adjusted) and reapplies
 * them when a matching node reappears or after a panel restart.
 *
 * State file: isde_xdg_config_path("audio-state.toml").
 *
 * Per-node state is stored as flat top-level TOML tables whose keys embed
 * the node_name, e.g. ["sinks.alsa_output.pci-0000_00_1f.3.hdmi-stereo"].
 * Quoting the whole key makes tomlc99 treat it as a single (dotted) key
 * rather than a nested table path, so isde_config_table(root, key) finds
 * it directly. The isde-config-write API writes the section header as
 * [<section>], so we pass the key wrapped in literal double quotes.
 */
#include "tray-audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "isde-config.h"
#include "isde-xdg.h"

/* ---------- in-memory per-node cache ---------- */

typedef struct ManualNode {
    char  node_name[256];
    float volume;       /* linear 0.0-1.0 */
    int   muted;
    int   have_volume;
    int   have_mute;
} ManualNode;

static ManualNode manual_sinks[MAX_SINKS];
static int        n_manual_sinks = 0;
static ManualNode manual_sources[MAX_SOURCES];
static int        n_manual_sources = 0;

static ManualNode *manual_find(ManualNode *arr, int n, const char *node_name)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(arr[i].node_name, node_name) == 0)
            return &arr[i];
    }
    return NULL;
}

static ManualNode *manual_add(ManualNode *arr, int *n, int cap,
                              const char *node_name)
{
    ManualNode *m = manual_find(arr, *n, node_name);
    if (m)
        return m;
    if (*n >= cap)
        return NULL;
    m = &arr[*n];
    memset(m, 0, sizeof(*m));
    snprintf(m->node_name, sizeof(m->node_name), "%s", node_name);
    (*n)++;
    return m;
}

/* ---------- load ---------- */

void ta_state_load(TrayAudio *ta)
{
    char *path = isde_xdg_config_path("audio-state.toml");
    if (!path)
        return;

    char err[256] = {0};
    IsdeConfig *cfg = isde_config_load(path, err, sizeof(err));
    if (!cfg) {
        free(path);
        return;
    }

    IsdeConfigTable *root = isde_config_root(cfg);

    /* Defaults */
    IsdeConfigTable *defaults = isde_config_table(root, "defaults");
    if (defaults) {
        const char *ds = isde_config_string(defaults, "default_sink", NULL);
        if (ds) {
            ta->manual_default_sink = strdup(ds);
            free((char *)ds);
        }
        const char *ss = isde_config_string(defaults, "default_source", NULL);
        if (ss) {
            ta->manual_default_source = strdup(ss);
            free((char *)ss);
        }
    }

    /* Per-node tables. Each is a top-level table keyed "sinks.<name>" or
     * "sources.<name>". Iterate root's sub-tables. */
    int n = isde_config_table_count(root);
    for (int i = 0; i < n; i++) {
        const char *key = isde_config_table_key(root, i);
        if (!key)
            continue;

        int is_source = 0;
        const char *nn = NULL;
        if (strncmp(key, "sinks.", 6) == 0) {
            nn = key + 6;
            is_source = 0;
        } else if (strncmp(key, "sources.", 8) == 0) {
            nn = key + 8;
            is_source = 1;
        } else {
            continue;  /* e.g. "defaults" */
        }
        if (!nn[0])
            continue;

        IsdeConfigTable *t = isde_config_table(root, key);
        if (!t)
            continue;

        ManualNode *m = is_source
            ? manual_add(manual_sources, &n_manual_sources, MAX_SOURCES, nn)
            : manual_add(manual_sinks, &n_manual_sinks, MAX_SINKS, nn);
        if (!m)
            continue;

        double vol = isde_config_double(t, "volume", -1.0);
        if (vol >= 0.0) {
            m->volume = (float)vol;
            m->have_volume = 1;
        }
        /* isde_config_bool returns the default (here -1) when absent; use a
         * sentinel default we can detect. */
        int have_mute = 0;
        int mv = isde_config_bool(t, "muted", -1);
        if (mv != -1) {
            have_mute = 1;
            m->muted = mv;
        }
        m->have_mute = have_mute;
    }

    isde_config_free(cfg);
    free(path);
}

/* ---------- save ---------- */

static void state_save_to_disk(TrayAudio *ta)
{
    char *path = isde_xdg_config_path("audio-state.toml");
    if (!path)
        return;

    /* Defaults section */
    if (ta->manual_default_sink)
        isde_config_write_string(path, "defaults", "default_sink",
                                 ta->manual_default_sink);
    if (ta->manual_default_source)
        isde_config_write_string(path, "defaults", "default_source",
                                 ta->manual_default_source);

    /* Per-node sections. Wrap the key in literal quotes so the writer emits
     * ["sinks.<name>"] / ["sources.<name>"], which tomlc99 parses as a single
     * top-level table keyed "sinks.<name>". */
    for (int i = 0; i < n_manual_sinks; i++) {
        ManualNode *m = &manual_sinks[i];
        char sec[300];
        snprintf(sec, sizeof(sec), "\"sinks.%s\"", m->node_name);
        if (m->have_volume)
            isde_config_write_double(path, sec, "volume", (double)m->volume);
        if (m->have_mute)
            isde_config_write_bool(path, sec, "muted", m->muted);
    }
    for (int i = 0; i < n_manual_sources; i++) {
        ManualNode *m = &manual_sources[i];
        char sec[300];
        snprintf(sec, sizeof(sec), "\"sources.%s\"", m->node_name);
        if (m->have_volume)
            isde_config_write_double(path, sec, "volume", (double)m->volume);
        if (m->have_mute)
            isde_config_write_bool(path, sec, "muted", m->muted);
    }

    free(path);
}

static void save_timer_cb(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    TrayAudio *ta = (TrayAudio *)client_data;
    ta->save_timer = 0;
    ta->save_pending = 0;
    state_save_to_disk(ta);
}

void ta_state_schedule_save(TrayAudio *ta)
{
    if (!ta->panel || !ta->panel->app)
        return;
    if (ta->save_timer)
        IswRemoveTimeOut(ta->save_timer);
    ta->save_timer = IswAppAddTimeOut(ta->panel->app, 1000,
                                      save_timer_cb, ta);
    ta->save_pending = 1;
}

void ta_state_save_now(TrayAudio *ta)
{
    if (ta->save_timer) {
        IswRemoveTimeOut(ta->save_timer);
        ta->save_timer = 0;
    }
    if (ta->save_pending) {
        ta->save_pending = 0;
        state_save_to_disk(ta);
    }
}

void ta_state_cleanup(TrayAudio *ta)
{
    if (ta->save_timer) {
        IswRemoveTimeOut(ta->save_timer);
        ta->save_timer = 0;
    }
    ta->save_pending = 0;
    free(ta->manual_default_sink);
    ta->manual_default_sink = NULL;
    free(ta->manual_default_source);
    ta->manual_default_source = NULL;
    n_manual_sinks = 0;
    n_manual_sources = 0;
}

/* ---------- recording manual actions ---------- */

void ta_state_record_default_sink(TrayAudio *ta, const char *node_name)
{
    if (!node_name || !node_name[0])
        return;
    free(ta->manual_default_sink);
    ta->manual_default_sink = strdup(node_name);
    ta_state_schedule_save(ta);
}

void ta_state_record_default_source(TrayAudio *ta, const char *node_name)
{
    if (!node_name || !node_name[0])
        return;
    free(ta->manual_default_source);
    ta->manual_default_source = strdup(node_name);
    ta_state_schedule_save(ta);
}

void ta_state_record_volume(TrayAudio *ta, int is_source,
                            const char *node_name, float volume)
{
    if (!node_name || !node_name[0])
        return;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;

    ManualNode *m = is_source
        ? manual_add(manual_sources, &n_manual_sources, MAX_SOURCES, node_name)
        : manual_add(manual_sinks, &n_manual_sinks, MAX_SINKS, node_name);
    if (!m)
        return;
    m->volume = volume;
    m->have_volume = 1;
    ta_state_schedule_save(ta);
}

void ta_state_record_mute(TrayAudio *ta, int is_source,
                          const char *node_name, int muted)
{
    if (!node_name || !node_name[0])
        return;
    ManualNode *m = is_source
        ? manual_add(manual_sources, &n_manual_sources, MAX_SOURCES, node_name)
        : manual_add(manual_sinks, &n_manual_sinks, MAX_SINKS, node_name);
    if (!m)
        return;
    m->muted = muted ? 1 : 0;
    m->have_mute = 1;
    ta_state_schedule_save(ta);
}

/* ---------- reapply ---------- */

static SinkInfo *find_sink_by_name(TrayAudio *ta, const char *node_name)
{
    for (int i = 0; i < ta->nsinks; i++) {
        if (strcmp(ta->sinks[i].node_name, node_name) == 0)
            return &ta->sinks[i];
    }
    return NULL;
}

static SourceInfo *find_source_by_name(TrayAudio *ta, const char *node_name)
{
    for (int i = 0; i < ta->nsources; i++) {
        if (strcmp(ta->sources[i].node_name, node_name) == 0)
            return &ta->sources[i];
    }
    return NULL;
}

void ta_state_apply_for_node(TrayAudio *ta, int is_source, const char *node_name)
{
    if (!node_name || !node_name[0])
        return;

    ManualNode *m = is_source
        ? manual_find(manual_sources, n_manual_sources, node_name)
        : manual_find(manual_sinks, n_manual_sinks, node_name);
    if (!m)
        return;

    if (is_source) {
        SourceInfo *s = find_source_by_name(ta, node_name);
        if (!s)
            return;
        if (m->have_volume) {
            /* Guard: skip if already at this value (avoid loops). */
            if (fabsf(s->volume - m->volume) > 0.005f)
                ta_pw_set_volume(ta, s->id, m->volume);
        }
        if (m->have_mute) {
            if (s->muted != m->muted)
                ta_pw_set_mute(ta, s->id, m->muted);
        }
    } else {
        SinkInfo *s = find_sink_by_name(ta, node_name);
        if (!s)
            return;
        if (m->have_volume) {
            if (fabsf(s->volume - m->volume) > 0.005f)
                ta_pw_set_volume(ta, s->id, m->volume);
        }
        if (m->have_mute) {
            if (s->muted != m->muted)
                ta_pw_set_mute(ta, s->id, m->muted);
        }
    }
}

void ta_state_apply_defaults(TrayAudio *ta)
{
    if (ta->manual_default_sink) {
        SinkInfo *cur = ta_default_sink(ta);
        if (!cur || strcmp(cur->node_name, ta->manual_default_sink) != 0) {
            SinkInfo *target = find_sink_by_name(ta, ta->manual_default_sink);
            if (target) {
                fprintf(stderr,
                        "isde-panel: tray-audio: restoring default sink -> %s\n",
                        target->node_name);
                ta_pw_set_default_sink(ta, target->id);
            }
        }
    }

    if (ta->manual_default_source) {
        SourceInfo *cur = ta_default_source(ta);
        if (!cur || strcmp(cur->node_name, ta->manual_default_source) != 0) {
            SourceInfo *target =
                find_source_by_name(ta, ta->manual_default_source);
            if (target) {
                fprintf(stderr,
                        "isde-panel: tray-audio: restoring default source -> %s\n",
                        target->node_name);
                ta_pw_set_default_source(ta, target->id);
            }
        }
    }
}
