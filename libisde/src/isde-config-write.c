#define _POSIX_C_SOURCE 200809L
/*
 * isde-config-write.c — TOML config file writer
 *
 * Reads an existing TOML file, replaces or inserts a key within
 * a [section], and writes it back. Preserves comments and formatting
 * for lines it doesn't touch.
 */
#include "isde/isde-config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

/* Read entire file into a malloc'd string. Returns NULL on failure. */
static char *read_file(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (len < 0) { fclose(fp); return NULL; }

    char *buf = malloc(len + 1);
    if (!buf) { fclose(fp); return NULL; }

    size_t nread = fread(buf, 1, len, fp);
    buf[nread] = '\0';
    fclose(fp);
    return buf;
}

/* Ensure parent directory exists */
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

/*
 * Write a key=value pair into a TOML file at the given section.
 * value_str is the already-formatted TOML value (e.g. "\"hello\"", "42", "true").
 *
 * Strategy:
 *   1. Read the file line by line
 *   2. Find [section] header
 *   3. Within the section, find "key = ..." and replace the line
 *   4. If key not found in section, append it at the end of the section
 *   5. If section not found, append [section] + key at the end
 *   6. Write back
 */
static int write_value(const char *path, const char *section,
                       const char *key, const char *value_str)
{
    ensure_dir(path);

    char *content = read_file(path);
    /* If file doesn't exist, create from scratch */
    if (!content) {
        FILE *fp = fopen(path, "w");
        if (!fp) return -1;
        fprintf(fp, "[%s]\n%s = %s\n", section, key, value_str);
        fclose(fp);
        return 0;
    }

    /* Parse line by line */
    FILE *out = fopen(path, "w");
    if (!out) { free(content); return -1; }

    int in_target_section = 0;
    int key_written = 0;
    int section_found = 0;
    size_t key_len = strlen(key);

    char *line = content;
    while (line && *line) {
        char *eol = strchr(line, '\n');
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);

        /* Temporary null-terminate for comparison */
        char saved = line[line_len];
        line[line_len] = '\0';

        /* Strip leading whitespace for comparison */
        const char *trimmed = line;
        while (*trimmed && isspace((unsigned char)*trimmed))
            trimmed++;

        if (trimmed[0] == '[') {
            /* Section header */
            if (in_target_section && !key_written) {
                /* End of target section without finding key — insert before next section */
                fprintf(out, "%s = %s\n", key, value_str);
                key_written = 1;
            }

            /* Check if this is our target section */
            char section_check[256];
            snprintf(section_check, sizeof(section_check), "[%s]", section);
            in_target_section = (strncmp(trimmed, section_check,
                                         strlen(section_check)) == 0);
            if (in_target_section)
                section_found = 1;

        } else if (in_target_section && !key_written && trimmed[0] != '#'
                   && trimmed[0] != '\0') {
            /* Check if this line is our key */
            if (strncmp(trimmed, key, key_len) == 0) {
                const char *after = trimmed + key_len;
                while (*after && isspace((unsigned char)*after))
                    after++;
                if (*after == '=') {
                    /* Replace this line */
                    fprintf(out, "%s = %s\n", key, value_str);
                    key_written = 1;
                    line[line_len] = saved;
                    line = eol ? eol + 1 : NULL;
                    continue;
                }
            }
        }

        /* Write the original line */
        line[line_len] = saved;
        fwrite(line, 1, line_len, out);
        fputc('\n', out);

        line = eol ? eol + 1 : NULL;
    }

    /* If we were in the target section at EOF and didn't write the key */
    if (in_target_section && !key_written) {
        fprintf(out, "%s = %s\n", key, value_str);
        key_written = 1;
    }

    /* If section was never found, append it */
    if (!section_found) {
        fprintf(out, "\n[%s]\n%s = %s\n", section, key, value_str);
    }

    fclose(out);
    free(content);
    return 0;
}

/* ---------- public API ---------- */

int isde_config_write_string(const char *path, const char *section,
                              const char *key, const char *value)
{
    /* Format as TOML string: "value" */
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

