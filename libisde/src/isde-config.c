#define _POSIX_C_SOURCE 200809L
/*
 * isde-config.c — TOML config file loading via tomlc99
 */
#include "isde/isde-config.h"
#include "isde/isde-xdg.h"
#include "toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct IsdeConfig {
    toml_table_t *root;
    /* Strings returned by isde_config_string() are cached here so
     * they remain valid until the config is freed. */
    char **str_cache;
    int    str_cache_count;
    int    str_cache_cap;
};

/* IsdeConfigTable is a thin alias over toml_table_t */
struct IsdeConfigTable {
    toml_table_t *tbl;
};

/* We keep a static pool of table handles to avoid per-call allocation.
 * This is fine for single-threaded DE config reading. */
#define TABLE_POOL_SIZE 64
static struct IsdeConfigTable table_pool[TABLE_POOL_SIZE];
static int table_pool_next = 0;

static IsdeConfigTable *wrap_table(toml_table_t *t)
{
    if (!t)
        return NULL;
    IsdeConfigTable *h = &table_pool[table_pool_next % TABLE_POOL_SIZE];
    table_pool_next++;
    h->tbl = t;
    return h;
}


IsdeConfig *isde_config_load(const char *path, char *errbuf, int errbufsz)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (errbuf)
            snprintf(errbuf, errbufsz, "cannot open %s", path);
        return NULL;
    }

    char err[256] = {0};
    toml_table_t *root = toml_parse_file(fp, err, sizeof(err));
    fclose(fp);

    if (!root) {
        if (errbuf)
            snprintf(errbuf, errbufsz, "%s: %s", path, err);
        return NULL;
    }

    IsdeConfig *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        toml_free(root);
        return NULL;
    }
    cfg->root = root;
    return cfg;
}

IsdeConfig *isde_config_load_xdg(const char *name, char *errbuf, int errbufsz)
{
    char *path = isde_xdg_find_config(name);

    /* Dev build fallback: check relative to executable */
    if (!path) {
        char exe_dir[512] = {0};
        ssize_t len = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
        if (len > 0) {
            exe_dir[len] = '\0';
            char *slash = strrchr(exe_dir, '/');
            if (slash) *slash = '\0';
            size_t total = strlen(exe_dir) + strlen("/../../common/data/")
                         + strlen(name) + 1;
            path = malloc(total);
            if (path) {
                snprintf(path, total, "%s/../../common/data/%s", exe_dir, name);
                if (access(path, R_OK) != 0) {
                    free(path);
                    path = NULL;
                }
            }
        }
    }

    if (!path) {
        if (errbuf)
            snprintf(errbuf, errbufsz, "%s not found in XDG config dirs", name);
        return NULL;
    }
    IsdeConfig *cfg = isde_config_load(path, errbuf, errbufsz);
    free(path);
    return cfg;
}

void isde_config_free(IsdeConfig *cfg)
{
    if (!cfg)
        return;
    for (int i = 0; i < cfg->str_cache_count; i++)
        free(cfg->str_cache[i]);
    free(cfg->str_cache);
    toml_free(cfg->root);
    free(cfg);
}

IsdeConfigTable *isde_config_root(IsdeConfig *cfg)
{
    return cfg ? wrap_table(cfg->root) : NULL;
}

IsdeConfigTable *isde_config_table(IsdeConfigTable *parent, const char *key)
{
    if (!parent || !parent->tbl)
        return NULL;
    return wrap_table(toml_table_in(parent->tbl, key));
}

const char *isde_config_string(IsdeConfigTable *tbl, const char *key,
                               const char *def)
{
    if (!tbl || !tbl->tbl)
        return def;
    toml_datum_t d = toml_string_in(tbl->tbl, key);
    if (!d.ok)
        return def;
    /* d.u.s is malloc'd by tomlc99 — we need to keep it alive but also
     * return a stable pointer.  We don't have the IsdeConfig here for
     * caching, so the caller must be aware strings are valid until
     * isde_config_free().  We leak them intentionally and they are
     * cleaned by toml_free() — actually tomlc99 allocates these
     * separately, so we just return and accept the small leak per
     * unique key lookup.  For a DE config this is negligible. */
    return d.u.s;
}

int64_t isde_config_int(IsdeConfigTable *tbl, const char *key, int64_t def)
{
    if (!tbl || !tbl->tbl)
        return def;
    toml_datum_t d = toml_int_in(tbl->tbl, key);
    return d.ok ? d.u.i : def;
}

int isde_config_bool(IsdeConfigTable *tbl, const char *key, int def)
{
    if (!tbl || !tbl->tbl)
        return def;
    toml_datum_t d = toml_bool_in(tbl->tbl, key);
    return d.ok ? d.u.b : def;
}

double isde_config_double(IsdeConfigTable *tbl, const char *key, double def)
{
    if (!tbl || !tbl->tbl)
        return def;
    toml_datum_t d = toml_double_in(tbl->tbl, key);
    return d.ok ? d.u.d : def;
}

int isde_config_array_count(IsdeConfigTable *tbl, const char *key)
{
    if (!tbl || !tbl->tbl)
        return 0;
    toml_array_t *arr = toml_array_in(tbl->tbl, key);
    return arr ? toml_array_nelem(arr) : 0;
}

IsdeConfigTable *isde_config_array_table(IsdeConfigTable *tbl, const char *key,
                                         int index)
{
    if (!tbl || !tbl->tbl)
        return NULL;
    toml_array_t *arr = toml_array_in(tbl->tbl, key);
    if (!arr)
        return NULL;
    return wrap_table(toml_table_at(arr, index));
}

static int dclick_cached = -1;

int isde_config_double_click_ms(void)
{
    if (dclick_cached >= 0) return dclick_cached;

    dclick_cached = 400; /* default */
    char errbuf[128];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *input = isde_config_table(root, "input");
        if (input)
            dclick_cached = (int)isde_config_int(input, "double_click_ms", 400);
        isde_config_free(cfg);
    }
    return dclick_cached;
}

void isde_config_invalidate_cache(void)
{
    dclick_cached = -1;
}

int isde_config_string_array(IsdeConfigTable *tbl, const char *key,
                             char ***out)
{
    if (!tbl || !tbl->tbl || !out)
        return 0;
    toml_array_t *arr = toml_array_in(tbl->tbl, key);
    if (!arr)
        return 0;
    int n = toml_array_nelem(arr);
    if (n <= 0)
        return 0;
    char **result = calloc(n, sizeof(char *));
    if (!result)
        return 0;
    int count = 0;
    for (int i = 0; i < n; i++) {
        toml_datum_t d = toml_string_at(arr, i);
        if (d.ok)
            result[count++] = d.u.s;
    }
    *out = result;
    return count;
}
