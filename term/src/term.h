/*
 * term.h — internal header for isde-term
 */
#ifndef ISDE_TERM_H
#define ISDE_TERM_H

#include <ISW/Intrinsic.h>
#include <ISW/IntrinsicP.h>
#include <ISW/StringDefs.h>
#include <ISW/DrawingArea.h>
#include <ISW/ISWRender.h>

#include <stdbool.h>
#include <stdint.h>

#include "isde/isde-config.h"
#include "isde/isde-dbus.h"
#include "isde/isde-theme.h"

struct tsm_screen;
struct tsm_vte;
struct TermPty;
struct TermWidget;

/* 16 ANSI + FG + BG */
#define TERM_PALETTE_N 18

typedef struct {
    uint8_t rgb[TERM_PALETTE_N][3];
    uint8_t cursor[3];
    bool    cursor_set;
} TermPalette;

typedef struct {
    char        font_family[128];
    int         font_size;         /* pt */
    char        color_scheme[128]; /* theme name */
    int         scrollback;
    char        cursor_shape[16];  /* "block" | "underline" | "bar" */
    TermPalette palette;
} TermConfig;

/* ---- config ---- */
void term_config_load(TermConfig *cfg);
bool term_config_load_palette(const char *theme_name, TermPalette *out);

/* ---- PTY ---- */
typedef void (*TermPtyReadCb)(const char *buf, size_t n, void *user);
typedef void (*TermPtyExitCb)(int status, void *user);

typedef struct TermPty TermPty;

TermPty *term_pty_spawn(IswAppContext app,
                        const char *shell, char *const *argv,
                        unsigned cols, unsigned rows,
                        TermPtyReadCb on_read,
                        TermPtyExitCb on_exit,
                        void *user);
void     term_pty_write(TermPty *p, const char *data, size_t n);
void     term_pty_resize(TermPty *p, unsigned cols, unsigned rows,
                         unsigned px_w, unsigned px_h);
void     term_pty_close(TermPty *p);

/* ---- Widget (created via DrawingArea + glue) ---- */
typedef struct TermWidget TermWidget;

TermWidget *term_widget_create(Widget parent, const char *name,
                               const TermConfig *cfg,
                               int cols, int rows);
void        term_widget_destroy(TermWidget *t);
Widget      term_widget_canvas(TermWidget *t);
void        term_widget_attach_pty(TermWidget *t, TermPty *pty);
void        term_widget_feed(TermWidget *t, const char *buf, size_t n);
void        term_widget_invalidate(TermWidget *t);
void        term_widget_apply_config(TermWidget *t, const TermConfig *cfg);
void        term_widget_preferred_pixels(TermWidget *t,
                                         int cols, int rows,
                                         int *px_w, int *px_h);

#endif /* ISDE_TERM_H */
