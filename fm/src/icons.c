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

/* Load an icon by name without fallback. Returns malloc'd SVG or NULL. */
static char *try_load_mime_icon(const char *theme, const char *name)
{
    if (theme) {
        char *path = isde_icon_theme_lookup(theme, "mimetypes", name);
        if (path) {
            char *data = read_svg_file(path);
            free(path);
            if (data) { return data; }
        }
    }
    char *path = isde_icon_find("mimetypes", name);
    if (path) {
        char *data = read_svg_file(path);
        free(path);
        if (data) { return data; }
    }
    return NULL;
}

/* Resolve an icon for a MIME type using freedesktop naming:
 * "media/sub" -> "media-sub", fallback "media-x-generic".
 * Caches per-mime results on the FmApp (SVG blob or NULL sentinel). */
static const char *mime_icon_lookup(FmApp *app, const char *mime)
{
    for (int i = 0; i < app->nmime_icons; i++) {
        if (strcmp(app->mime_icons[i].mime, mime) == 0) {
            return app->mime_icons[i].svg;
        }
    }

    const char *slash = strchr(mime, '/');
    char *svg = NULL;
    if (slash) {
        size_t mlen = slash - mime;
        size_t slen = strlen(slash + 1);
        char *name = malloc(mlen + 1 + slen + 1);
        if (name) {
            memcpy(name, mime, mlen);
            name[mlen] = '-';
            memcpy(name + mlen + 1, slash + 1, slen + 1);
            svg = try_load_mime_icon(app->icon_theme, name);
            if (!svg) {
                /* Fallback: <media>-x-generic */
                char *gen = malloc(mlen + strlen("-x-generic") + 1);
                if (gen) {
                    memcpy(gen, mime, mlen);
                    strcpy(gen + mlen, "-x-generic");
                    svg = try_load_mime_icon(app->icon_theme, gen);
                    free(gen);
                }
            }
            free(name);
        }
    }

    if (app->nmime_icons >= app->cmime_icons) {
        int nc = app->cmime_icons ? app->cmime_icons * 2 : 16;
        void *n = realloc(app->mime_icons, nc * sizeof(*app->mime_icons));
        if (!n) { free(svg); return NULL; }
        app->mime_icons = n;
        app->cmime_icons = nc;
    }
    app->mime_icons[app->nmime_icons].mime = strdup(mime);
    app->mime_icons[app->nmime_icons].svg = svg;
    app->nmime_icons++;
    return svg;
}

const char *icons_for_entry(FmApp *app, const FmEntry *e)
{
    if (e->is_dir) {
        return app->icon_folder;
    }

    const char *mime = isde_mime_type_for_file(e->name);
    if (mime && strcmp(mime, "application/octet-stream") != 0) {
        const char *svg = mime_icon_lookup(app, mime);
        if (svg) { return svg; }

        /* Generic fallback by media type */
        if (strncmp(mime, "image/", 6) == 0) {
            return app->icon_image;
        }
    }

    if (e->mode & S_IXUSR) {
        return app->icon_exec;
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

    for (int i = 0; i < app->nmime_icons; i++) {
        free(app->mime_icons[i].mime);
        free(app->mime_icons[i].svg);
    }
    free(app->mime_icons);
    app->mime_icons = NULL;
    app->nmime_icons = 0;
    app->cmime_icons = 0;
}
