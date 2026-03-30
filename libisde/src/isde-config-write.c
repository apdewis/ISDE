#define _POSIX_C_SOURCE 200809L
/*
 * isde-config-write.c — TOML config file writer
 *
 * Reads an existing TOML file line by line, replaces or inserts a
 * key within a [section], and writes it back.
 */
#include "isde/isde-config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

static void ensure_dir(const char *path)
{
    char *dir = strdup(path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0755);
    }
    free(dir);
}


static int write_value(const char *path, const char *section,
                       const char *key, const char *value_str)
{
    ensure_dir(path);

    FILE *in = fopen(path, "r");
    if (!in) {
        /* File doesn't exist — create from scratch */
        FILE *fp = fopen(path, "w");
        if (!fp) { return -1; }
        fprintf(fp, "[%s]\n%s = %s\n", section, key, value_str);
        fclose(fp);
        return 0;
    }

    /* Read all lines into memory */
    char **lines = NULL;
    int nlines = 0;
    int cap = 64;
    lines = malloc(cap * sizeof(char *));

    char buf[1024];
    while (fgets(buf, sizeof(buf), in)) {
        /* Strip trailing newline */
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') {
            buf[len - 1] = '\0';
        }
        if (nlines >= cap) {
            cap *= 2;
            lines = realloc(lines, cap * sizeof(char *));
        }
        lines[nlines++] = strdup(buf);
    }
    fclose(in);

    /* Find section and key, mark for replacement */
    int in_target = 0;
    int key_line = -1;    /* line index where key was found */
    int section_end = -1; /* line index where target section ends */
    int section_start = -1;
    size_t key_len = strlen(key);
    char section_header[256];
    snprintf(section_header, sizeof(section_header), "[%s]", section);
    size_t sh_len = strlen(section_header);

    for (int i = 0; i < nlines; i++) {
        const char *trimmed = lines[i];
        while (*trimmed && isspace((unsigned char)*trimmed)) {
            trimmed++;
        }

        if (trimmed[0] == '[') {
            if (in_target && key_line < 0) {
                section_end = i; /* key not found, insert before this section */
            }
            in_target = (strncmp(trimmed, section_header, sh_len) == 0 &&
                        (trimmed[sh_len] == '\0' || isspace((unsigned char)trimmed[sh_len])));
            if (in_target) {
                section_start = i;
            }
        } else if (in_target && key_line < 0 &&
                   trimmed[0] != '#' && trimmed[0] != '\0') {
            if (strncmp(trimmed, key, key_len) == 0) {
                const char *after = trimmed + key_len;
                while (*after && isspace((unsigned char)*after)) {
                    after++;
                }
                if (*after == '=') {
                    key_line = i;
                }
            }
        }
    }
    if (in_target && section_end < 0) {
        section_end = nlines; /* section goes to end of file */
    }

    /* Write output */
    FILE *out = fopen(path, "w");
    if (!out) {
        for (int i = 0; i < nlines; i++) { free(lines[i]); }
        free(lines);
        return -1;
    }

    int written = 0;
    for (int i = 0; i < nlines; i++) {
        if (i == key_line) {
            /* Replace this line */
            fprintf(out, "%s = %s\n", key, value_str);
            written = 1;
        } else if (i == section_end && key_line < 0 && section_start >= 0) {
            /* Insert key before the next section header */
            fprintf(out, "%s = %s\n", key, value_str);
            fprintf(out, "%s\n", lines[i]);
            written = 1;
        } else {
            fprintf(out, "%s\n", lines[i]);
        }
    }

    /* Key not found and section ends at EOF */
    if (!written && section_start >= 0 && key_line < 0) {
        fprintf(out, "%s = %s\n", key, value_str);
    }

    /* Section not found at all */
    if (section_start < 0) {
        fprintf(out, "\n[%s]\n%s = %s\n", section, key, value_str);
    }

    fclose(out);
    for (int i = 0; i < nlines; i++) { free(lines[i]); }
    free(lines);
    return 0;
}

/* ---------- public API ---------- */

int isde_config_write_string(const char *path, const char *section,
                              const char *key, const char *value)
{
    size_t len = strlen(value) + 3;
    char *val_str = malloc(len);
    snprintf(val_str, len, "\"%s\"", value);
    int ret = write_value(path, section, key, val_str);
    free(val_str);
    return ret;
}

int isde_config_write_int(const char *path, const char *section,
                           const char *key, int64_t value)
{
    char val_str[32];
    snprintf(val_str, sizeof(val_str), "%ld", (long)value);
    return write_value(path, section, key, val_str);
}

int isde_config_write_bool(const char *path, const char *section,
                            const char *key, int value)
{
    return write_value(path, section, key, value ? "true" : "false");
}
