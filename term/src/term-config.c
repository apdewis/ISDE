#define _POSIX_C_SOURCE 200809L
#include "term.h"
#include "isde/isde-config.h"
#include "isde/isde-theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void color_to_rgb(unsigned int c, uint8_t out[3])
{
    out[0] = (c >> 16) & 0xff;
    out[1] = (c >>  8) & 0xff;
    out[2] =  c        & 0xff;
}

static void fill_palette_from_scheme(const IsdeColorScheme *s, TermPalette *p)
{
    if (!s) {
        memset(p, 0, sizeof(*p));
        /* sensible built-in defaults (dark) */
        static const unsigned int ansi[16] = {
            0x000000,0xCD3131,0x0DBC79,0xE5E510,0x2472C8,0xBC3FBC,0x11A8CD,0xE5E5E5,
            0x666666,0xF14C4C,0x23D18B,0xF5F543,0x3B8EEA,0xD670D6,0x29B8DB,0xFFFFFF
        };
        for (int i = 0; i < 16; i++) color_to_rgb(ansi[i], p->rgb[i]);
        color_to_rgb(0xE5E5E5, p->rgb[16]); /* FG */
        color_to_rgb(0x1E1E2E, p->rgb[17]); /* BG */
        color_to_rgb(0xE5E5E5, p->cursor);
        p->cursor_set = true;
        return;
    }
    for (int i = 0; i < 16; i++) color_to_rgb(s->terminal_ansi[i], p->rgb[i]);
    color_to_rgb(s->terminal_fg, p->rgb[16]);
    color_to_rgb(s->terminal_bg, p->rgb[17]);
    color_to_rgb(s->terminal_cursor, p->cursor);
    p->cursor_set = true;
}

bool term_config_load_palette(const char *theme_name, TermPalette *out)
{
    IsdeColorScheme *s = NULL;
    if (theme_name && theme_name[0]) {
        s = isde_scheme_load(theme_name);
    }
    if (!s) {
        const IsdeColorScheme *cur = isde_theme_current();
        if (cur) {
            fill_palette_from_scheme(cur, out);
            return true;
        }
        fill_palette_from_scheme(NULL, out);
        return false;
    }
    fill_palette_from_scheme(s, out);
    isde_scheme_free(s);
    return true;
}

void term_config_load(TermConfig *cfg)
{
    /* Defaults */
    snprintf(cfg->font_family, sizeof(cfg->font_family), "%s", "Monospace");
    cfg->font_size = 11;
    cfg->scrollback = 10000;
    snprintf(cfg->cursor_shape, sizeof(cfg->cursor_shape), "%s", "block");
    cfg->color_scheme[0] = '\0';

    char errbuf[256];
    IsdeConfig *c = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));

    /* Fall back to [fonts] fixed_family for the monospace default */
    if (c) {
        IsdeConfigTable *root = isde_config_root(c);
        IsdeConfigTable *fonts = isde_config_table(root, "fonts");
        if (fonts) {
            const char *ff = isde_config_string(fonts, "fixed_family", NULL);
            int fs = (int)isde_config_int(fonts, "fixed_size", 0);
            if (ff) snprintf(cfg->font_family, sizeof(cfg->font_family), "%s", ff);
            if (fs > 0) cfg->font_size = fs;
        }
        IsdeConfigTable *term = isde_config_table(root, "terminal");
        if (term) {
            const char *ff = isde_config_string(term, "font_family", NULL);
            int fs = (int)isde_config_int(term, "font_size", 0);
            const char *cs = isde_config_string(term, "color_scheme", NULL);
            int sb = (int)isde_config_int(term, "scrollback", -1);
            const char *csh = isde_config_string(term, "cursor_shape", NULL);
            if (ff) snprintf(cfg->font_family, sizeof(cfg->font_family), "%s", ff);
            if (fs > 0) cfg->font_size = fs;
            if (cs) snprintf(cfg->color_scheme, sizeof(cfg->color_scheme), "%s", cs);
            if (sb >= 0) cfg->scrollback = sb;
            if (csh) snprintf(cfg->cursor_shape, sizeof(cfg->cursor_shape), "%s", csh);
        }
        /* If no terminal.color_scheme, inherit [appearance].color_scheme */
        if (!cfg->color_scheme[0]) {
            IsdeConfigTable *app = isde_config_table(root, "appearance");
            if (app) {
                const char *cs = isde_config_string(app, "color_scheme", NULL);
                if (cs) snprintf(cfg->color_scheme, sizeof(cfg->color_scheme), "%s", cs);
            }
        }
        isde_config_free(c);
    }

    term_config_load_palette(cfg->color_scheme, &cfg->palette);
}
