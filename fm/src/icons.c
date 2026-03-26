#define _POSIX_C_SOURCE 200809L
/*
 * icons.c — icon resolution for file entries
 *
 * Loads SVG icons from the ISDE theme. Falls back to embedded SVG
 * if theme icons are not found.
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cached icon paths/data */
static char *icon_folder_data = NULL;
static char *icon_file_data   = NULL;
static char *icon_exec_data   = NULL;
static char *icon_image_data  = NULL;

/* Embedded fallbacks */
static const char *FALLBACK_FOLDER =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 48 48'>"
    "<path d='M4 8h16l4 4h20v28H4z' fill='#FFA726'/>"
    "<path d='M4 16h40v24H4z' fill='#FFCC80'/>"
    "</svg>";

static const char *FALLBACK_FILE =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 48 48'>"
    "<path d='M8 4h20l12 12v28H8z' fill='#E0E0E0'/>"
    "<path d='M28 4l12 12H28z' fill='#BDBDBD'/>"
    "</svg>";

static const char *FALLBACK_EXEC =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 48 48'>"
    "<path d='M8 4h20l12 12v28H8z' fill='#A5D6A7'/>"
    "<path d='M28 4l12 12H28z' fill='#66BB6A'/>"
    "<path d='M18 22l8 6-8 6z' fill='#2E7D32'/>"
    "</svg>";

/* Read an SVG file into a malloc'd string */
static char *read_svg_file(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (len <= 0 || len > 65536) {
        fclose(fp);
        return NULL;
    }

    char *buf = malloc(len + 1);
    if (!buf) { fclose(fp); return NULL; }

    size_t nread = fread(buf, 1, len, fp);
    buf[nread] = '\0';
    fclose(fp);
    return buf;
}

static char *configured_icon_theme = NULL;

static char *load_icon(const char *category, const char *name,
                       const char *fallback)
{
    /* Try configured icon theme first */
    if (configured_icon_theme) {
        char *path = isde_icon_theme_lookup(configured_icon_theme,
                                             category, name);
        if (path) {
            char *data = read_svg_file(path);
            free(path);
            if (data) return data;
        }
    }

    /* Try ISDE's own icon set */
    char *path = isde_icon_find(category, name);
    if (path) {
        char *data = read_svg_file(path);
        free(path);
        if (data) return data;
    }
    return strdup(fallback);
}

void icons_init(void)
{
    /* Free previous data if reloading */
    icons_cleanup();

    /* Read configured icon theme from config */
    free(configured_icon_theme);
    configured_icon_theme = NULL;
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *appear = isde_config_table(root, "appearance");
        if (appear) {
            const char *theme = isde_config_string(appear, "icon_theme", NULL);
            if (theme)
                configured_icon_theme = strdup(theme);
        }
        isde_config_free(cfg);
    }

    icon_folder_data = load_icon("places", "folder", FALLBACK_FOLDER);
    icon_file_data   = load_icon("mimetypes", "text-plain", FALLBACK_FILE);
    icon_exec_data   = load_icon("mimetypes", "application-x-executable",
                                 FALLBACK_EXEC);
    icon_image_data  = load_icon("mimetypes", "image-generic", FALLBACK_FILE);
}

const char *icons_for_entry(const FmEntry *e)
{
    if (e->is_dir)
        return icon_folder_data;
    if (e->mode & S_IXUSR)
        return icon_exec_data;

    /* Basic extension-based matching */
    const char *dot = strrchr(e->name, '.');
    if (dot) {
        if (strcmp(dot, ".png") == 0 || strcmp(dot, ".jpg") == 0 ||
            strcmp(dot, ".jpeg") == 0 || strcmp(dot, ".gif") == 0 ||
            strcmp(dot, ".svg") == 0 || strcmp(dot, ".bmp") == 0 ||
            strcmp(dot, ".webp") == 0)
            return icon_image_data;
    }

    return icon_file_data;
}

void icons_cleanup(void)
{
    free(icon_folder_data);
    free(icon_file_data);
    free(icon_exec_data);
    free(icon_image_data);
    icon_folder_data = NULL;
    icon_file_data = NULL;
    icon_exec_data = NULL;
    icon_image_data = NULL;
}
