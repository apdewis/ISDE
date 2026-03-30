#define _POSIX_C_SOURCE 200809L
/*
 * icons.c — icon resolution for file entries
 *
 * Loads SVG icons from the ISDE theme. Falls back to embedded SVG
 * if theme icons are not found. Icon data is stored in FmApp
 * and shared across all windows.
 */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    if (!fp) { return NULL; }

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

static char *load_icon(const char *theme, const char *category,
                       const char *name, const char *fallback)
{
    if (theme) {
        char *path = isde_icon_theme_lookup(theme, category, name);
        if (path) {
            char *data = read_svg_file(path);
            free(path);
            if (data) { return data; }
        }
    }

    char *path = isde_icon_find(category, name);
    if (path) {
        char *data = read_svg_file(path);
        free(path);
        if (data) { return data; }
    }
    return strdup(fallback);
}

void icons_init(FmApp *app)
{
    icons_cleanup(app);

    free(app->icon_theme);
    app->icon_theme = NULL;
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *appear = isde_config_table(root, "appearance");
        if (appear) {
            const char *theme = isde_config_string(appear, "icon_theme", NULL);
            if (theme) {
                app->icon_theme = strdup(theme);
            }
        }
        isde_config_free(cfg);
    }

    app->icon_folder = load_icon(app->icon_theme, "places", "folder",
                                 FALLBACK_FOLDER);
    app->icon_file   = load_icon(app->icon_theme, "mimetypes", "text-plain",
                                 FALLBACK_FILE);
    app->icon_exec   = load_icon(app->icon_theme, "mimetypes",
                                 "application-x-executable", FALLBACK_EXEC);
    app->icon_image  = load_icon(app->icon_theme, "mimetypes", "image-generic",
                                 FALLBACK_FILE);
}

const char *icons_for_entry(FmApp *app, const FmEntry *e)
{
    if (e->is_dir) {
        return app->icon_folder;
    }
    if (e->mode & S_IXUSR) {
        return app->icon_exec;
    }

    const char *dot = strrchr(e->name, '.');
    if (dot) {
        if (strcmp(dot, ".png") == 0 || strcmp(dot, ".jpg") == 0 ||
            strcmp(dot, ".jpeg") == 0 || strcmp(dot, ".gif") == 0 ||
            strcmp(dot, ".svg") == 0 || strcmp(dot, ".bmp") == 0 ||
            strcmp(dot, ".webp") == 0) {
            return app->icon_image;
        }
    }

    return app->icon_file;
}

void icons_cleanup(FmApp *app)
{
    free(app->icon_folder);
    free(app->icon_file);
    free(app->icon_exec);
    free(app->icon_image);
    app->icon_folder = NULL;
    app->icon_file = NULL;
    app->icon_exec = NULL;
    app->icon_image = NULL;
}
