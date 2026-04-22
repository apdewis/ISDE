#define _POSIX_C_SOURCE 200809L
#include "term.h"

#include <ISW/Intrinsic.h>
#include <ISW/IntrinsicP.h>
#include <ISW/DrawingArea.h>
#include <ISW/StringDefs.h>
#include <ISW/IswArgMacros.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xcb.h>

#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#define XK_MISCELLANY
#define XK_LATIN1
#define XK_XKB_KEYS
#include <X11/keysymdef.h>

#include <libtsm.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward decls from term-tsm.c */
int  term_tsm_new(struct tsm_screen **s, struct tsm_vte **v,
                  tsm_vte_write_cb cb, void *user);
void term_tsm_apply_palette(struct tsm_vte *vte, const TermPalette *pal);

struct TermWidget;
static void suppress_bg_clear(struct TermWidget *t);

struct TermWidget {
    Widget      canvas;
    TermConfig  cfg;

    struct tsm_screen *screen;
    struct tsm_vte    *vte;
    TermPty           *pty;

    cairo_font_face_t *font_face;
    double             font_size_px;
    double             cell_w;
    double             cell_h;
    double             baseline;

    unsigned           cols;
    unsigned           rows;
    tsm_age_t          last_age;
    int                bg_suppressed;

    xcb_key_symbols_t *key_syms;

    /* selection (cell coords) */
    int  sel_active;
    int  sel_pressed;

    /* preferred initial geometry */
    int  initial_cols;
    int  initial_rows;
};

/* ---------- rendering ---------- */

static void recompute_metrics(TermWidget *t)
{
    t->font_size_px = (double)t->cfg.font_size * 96.0 / 72.0;

    if (t->font_face) {
        cairo_font_face_destroy(t->font_face);
        t->font_face = NULL;
    }
    t->font_face = cairo_toy_font_face_create(t->cfg.font_family,
                                              CAIRO_FONT_SLANT_NORMAL,
                                              CAIRO_FONT_WEIGHT_NORMAL);

    /* Measure cell size using a scratch cairo surface */
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                       16, 16);
    cairo_t *cr = cairo_create(surf);
    cairo_set_font_face(cr, t->font_face);
    cairo_set_font_size(cr, t->font_size_px);
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    cairo_text_extents_t te;
    cairo_text_extents(cr, "M", &te);
    t->cell_w = te.x_advance > 0 ? te.x_advance : fe.max_x_advance;
    if (t->cell_w <= 0) t->cell_w = t->font_size_px * 0.6;
    t->cell_h = fe.height > 0 ? fe.height : t->font_size_px * 1.2;
    t->baseline = fe.ascent;
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
}

typedef struct {
    TermWidget *t;
    cairo_t    *cr;
} DrawCtx;

static void rgb_unpack(const uint8_t in[3], double out[3])
{
    out[0] = in[0] / 255.0;
    out[1] = in[1] / 255.0;
    out[2] = in[2] / 255.0;
}

static int draw_cell_cb(struct tsm_screen *con, uint64_t id,
                        const uint32_t *ch, size_t len, unsigned int width,
                        unsigned int posx, unsigned int posy,
                        const struct tsm_screen_attr *attr,
                        tsm_age_t age, void *data)
{
    (void)con; (void)id; (void)age;
    DrawCtx *dc = (DrawCtx *)data;
    TermWidget *t = dc->t;
    cairo_t *cr = dc->cr;

    if (width == 0) return 0;

    double x = posx * t->cell_w;
    double y = posy * t->cell_h;
    double w = width * t->cell_w;
    double h = t->cell_h;

    uint8_t fr, fg, fb, br, bg_, bb;
    if (attr->fccode < 0) {
        fr = attr->fr; fg = attr->fg; fb = attr->fb;
    } else {
        int i = attr->fccode;
        if (i < 0 || i >= TERM_PALETTE_N) i = 16;
        fr = t->cfg.palette.rgb[i][0];
        fg = t->cfg.palette.rgb[i][1];
        fb = t->cfg.palette.rgb[i][2];
    }
    if (attr->bccode < 0) {
        br = attr->br; bg_ = attr->bg; bb = attr->bb;
    } else {
        int i = attr->bccode;
        if (i < 0 || i >= TERM_PALETTE_N) i = 17;
        br = t->cfg.palette.rgb[i][0];
        bg_ = t->cfg.palette.rgb[i][1];
        bb = t->cfg.palette.rgb[i][2];
    }
    if (attr->inverse) {
        uint8_t tr=fr,tg=fg,tb=fb;
        fr=br; fg=bg_; fb=bb;
        br=tr; bg_=tg; bb=tb;
    }

    cairo_set_source_rgb(cr, br/255.0, bg_/255.0, bb/255.0);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);

    if (len == 0 || ch[0] == 0) {
        if (attr->underline) {
            cairo_set_source_rgb(cr, fr/255.0, fg/255.0, fb/255.0);
            cairo_rectangle(cr, x, y + h - 1.5, w, 1.0);
            cairo_fill(cr);
        }
        return 0;
    }

    /* Convert UTF-32 codepoints to UTF-8 for cairo_show_text */
    char utf8[32];
    int n = 0;
    for (size_t k = 0; k < len && n + 4 < (int)sizeof(utf8); k++) {
        uint32_t c = ch[k];
        if (c < 0x80) {
            utf8[n++] = (char)c;
        } else if (c < 0x800) {
            utf8[n++] = 0xC0 | (c >> 6);
            utf8[n++] = 0x80 | (c & 0x3F);
        } else if (c < 0x10000) {
            utf8[n++] = 0xE0 | (c >> 12);
            utf8[n++] = 0x80 | ((c >> 6) & 0x3F);
            utf8[n++] = 0x80 | (c & 0x3F);
        } else {
            utf8[n++] = 0xF0 | (c >> 18);
            utf8[n++] = 0x80 | ((c >> 12) & 0x3F);
            utf8[n++] = 0x80 | ((c >> 6) & 0x3F);
            utf8[n++] = 0x80 | (c & 0x3F);
        }
    }
    utf8[n] = '\0';

    cairo_set_font_face(cr, t->font_face);
    cairo_set_font_size(cr, t->font_size_px);
    cairo_set_source_rgb(cr, fr/255.0, fg/255.0, fb/255.0);
    cairo_move_to(cr, x, y + t->baseline);
    cairo_show_text(cr, utf8);

    if (attr->underline) {
        cairo_rectangle(cr, x, y + h - 1.5, w, 1.0);
        cairo_fill(cr);
    }

    return 0;
}

static void draw_cursor(TermWidget *t, cairo_t *cr)
{
    unsigned flags = tsm_screen_get_flags(t->screen);
    if (flags & TSM_SCREEN_HIDE_CURSOR) return;
    unsigned cx = tsm_screen_get_cursor_x(t->screen);
    unsigned cy = tsm_screen_get_cursor_y(t->screen);
    double x = cx * t->cell_w;
    double y = cy * t->cell_h;
    double c[3];
    rgb_unpack(t->cfg.palette.cursor, c);
    cairo_set_source_rgba(cr, c[0], c[1], c[2], 0.6);
    if (strcmp(t->cfg.cursor_shape, "underline") == 0) {
        cairo_rectangle(cr, x, y + t->cell_h - 2.0, t->cell_w, 2.0);
    } else if (strcmp(t->cfg.cursor_shape, "bar") == 0) {
        cairo_rectangle(cr, x, y, 2.0, t->cell_h);
    } else {
        cairo_rectangle(cr, x, y, t->cell_w, t->cell_h);
    }
    cairo_fill(cr);
}

static void expose_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w;
    ISWDrawingCallbackData *d = (ISWDrawingCallbackData *)call;
    TermWidget *t = (TermWidget *)cd;
    cairo_t *cr = (cairo_t *)ISWRenderGetCairoContext(d->render_ctx);
    if (!cr) return;

    if (!t->bg_suppressed) {
        suppress_bg_clear(t);
        t->bg_suppressed = 1;
    }

    /* Clear with background */
    double bg[3];
    rgb_unpack(t->cfg.palette.rgb[17], bg);
    cairo_set_source_rgb(cr, bg[0], bg[1], bg[2]);
    cairo_paint(cr);

    DrawCtx ctx = { t, cr };
    t->last_age = tsm_screen_draw(t->screen, draw_cell_cb, &ctx);
    draw_cursor(t, cr);
}

static void suppress_bg_clear(TermWidget *t)
{
    xcb_connection_t *c = IswDisplay(t->canvas);
    xcb_window_t win = IswWindow(t->canvas);
    uint32_t values[1] = { XCB_BACK_PIXMAP_NONE };
    xcb_change_window_attributes(c, win, XCB_CW_BACK_PIXMAP, values);
    xcb_flush(c);
}

static void request_redraw(TermWidget *t)
{
    if (!IswIsRealized(t->canvas)) return;
    xcb_connection_t *c = IswDisplay(t->canvas);
    xcb_window_t win = IswWindow(t->canvas);
    xcb_clear_area(c, 1, win, 0, 0, 0, 0);
    xcb_flush(c);
}

/* ---------- resize ---------- */

static void resize_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)call;
    TermWidget *t = (TermWidget *)cd;
    Dimension pw, ph;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgWidth(&ab, &pw);
    IswArgHeight(&ab, &ph);
    IswGetValues(w, ab.args, ab.count);

    unsigned cols = pw > 0 && t->cell_w > 0 ? (unsigned)(pw / t->cell_w) : 80;
    unsigned rows = ph > 0 && t->cell_h > 0 ? (unsigned)(ph / t->cell_h) : 24;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols == t->cols && rows == t->rows) return;
    t->cols = cols;
    t->rows = rows;
    tsm_screen_resize(t->screen, cols, rows);
    if (t->pty) term_pty_resize(t->pty, cols, rows, pw, ph);
    request_redraw(t);
}

/* ---------- input ---------- */

static unsigned mods_to_tsm(uint16_t xstate)
{
    unsigned m = 0;
    if (xstate & XCB_MOD_MASK_SHIFT)   m |= TSM_SHIFT_MASK;
    if (xstate & XCB_MOD_MASK_LOCK)    m |= TSM_LOCK_MASK;
    if (xstate & XCB_MOD_MASK_CONTROL) m |= TSM_CONTROL_MASK;
    if (xstate & XCB_MOD_MASK_1)       m |= TSM_ALT_MASK;
    if (xstate & XCB_MOD_MASK_4)       m |= TSM_LOGO_MASK;
    return m;
}

static uint32_t keysym_to_unicode(xcb_keysym_t ks)
{
    /* Latin-1 range direct; extended XKB keysyms have Unicode in low 24 bits
     * when above 0x01000000 (X11 convention). */
    if (ks >= 0x20 && ks <= 0x7E) return ks;
    if (ks >= 0xA0 && ks <= 0xFF) return ks;
    if ((ks & 0xff000000) == 0x01000000) return ks & 0x00ffffff;
    return TSM_VTE_INVALID;
}

static void handle_key_press(TermWidget *t, xcb_key_press_event_t *kev)
{
    if (!t->key_syms) t->key_syms = xcb_key_symbols_alloc(IswDisplay(t->canvas));
    int col = (kev->state & XCB_MOD_MASK_SHIFT) ? 1 : 0;
    xcb_keysym_t sym = xcb_key_symbols_get_keysym(t->key_syms, kev->detail, col);
    if (sym == XCB_NO_SYMBOL)
        sym = xcb_key_symbols_get_keysym(t->key_syms, kev->detail, 0);

    /* Local hotkeys: Shift+PgUp/PgDn, Ctrl+Shift+C/V */
    if ((kev->state & XCB_MOD_MASK_SHIFT) && sym == XK_Prior) {
        tsm_screen_sb_page_up(t->screen, 1);
        request_redraw(t);
        return;
    }
    if ((kev->state & XCB_MOD_MASK_SHIFT) && sym == XK_Next) {
        tsm_screen_sb_page_down(t->screen, 1);
        request_redraw(t);
        return;
    }

    unsigned mods = mods_to_tsm(kev->state);
    uint32_t uc = keysym_to_unicode(sym);

    bool consumed = tsm_vte_handle_keyboard(t->vte, sym,
                                            uc == TSM_VTE_INVALID ? 0 : uc,
                                            mods, uc);
    if (consumed) {
        tsm_screen_sb_reset(t->screen);
        request_redraw(t);
    }
}

static void handle_button(TermWidget *t, xcb_button_press_event_t *bev, bool press)
{
    int cx = (int)(bev->event_x / t->cell_w);
    int cy = (int)(bev->event_y / t->cell_h);
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;

    if (bev->detail == 4 /* wheel up */) {
        if (!press) return;
        tsm_screen_sb_up(t->screen, 3);
        request_redraw(t);
        return;
    }
    if (bev->detail == 5 /* wheel down */) {
        if (!press) return;
        tsm_screen_sb_down(t->screen, 3);
        request_redraw(t);
        return;
    }

    if (bev->detail == 1 /* left */) {
        if (press) {
            tsm_screen_selection_start(t->screen, cx, cy);
            t->sel_pressed = 1;
            t->sel_active = 1;
        } else {
            t->sel_pressed = 0;
        }
        request_redraw(t);
        return;
    }

    if (bev->detail == 2 /* middle — paste PRIMARY */) {
        /* TODO: wire PRIMARY selection via ISW selection API */
        return;
    }
}

static void handle_motion(TermWidget *t, xcb_motion_notify_event_t *mev)
{
    if (!t->sel_pressed) return;
    int cx = (int)(mev->event_x / t->cell_w);
    int cy = (int)(mev->event_y / t->cell_h);
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    tsm_screen_selection_target(t->screen, cx, cy);
    request_redraw(t);
}

static void input_cb(Widget w, IswPointer cd, IswPointer call)
{
    (void)w;
    TermWidget *t = (TermWidget *)cd;
    ISWDrawingCallbackData *d = (ISWDrawingCallbackData *)call;
    if (!d->event) return;
    uint8_t type = d->event->response_type & ~0x80;
    switch (type) {
    case XCB_KEY_PRESS:
        handle_key_press(t, (xcb_key_press_event_t *)d->event);
        break;
    case XCB_BUTTON_PRESS:
        handle_button(t, (xcb_button_press_event_t *)d->event, true);
        break;
    case XCB_BUTTON_RELEASE:
        handle_button(t, (xcb_button_press_event_t *)d->event, false);
        break;
    case XCB_MOTION_NOTIFY:
        handle_motion(t, (xcb_motion_notify_event_t *)d->event);
        break;
    default: break;
    }
}

/* ---------- VTE write -> PTY ---------- */

static void vte_write_cb(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
    (void)vte;
    TermWidget *t = (TermWidget *)data;
    if (t->pty) term_pty_write(t->pty, u8, len);
}

/* ---------- public ---------- */

TermWidget *term_widget_create(Widget parent, const char *name,
                               const TermConfig *cfg,
                               int cols, int rows)
{
    TermWidget *t = calloc(1, sizeof(*t));
    t->cfg = *cfg;
    t->initial_cols = cols > 0 ? cols : 80;
    t->initial_rows = rows > 0 ? rows : 24;
    t->cols = t->initial_cols;
    t->rows = t->initial_rows;

    if (term_tsm_new(&t->screen, &t->vte, vte_write_cb, t) < 0) {
        free(t);
        return NULL;
    }
    tsm_screen_set_max_sb(t->screen, cfg->scrollback);
    term_tsm_apply_palette(t->vte, &cfg->palette);

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgBorderWidth(&ab, 0);
    /* Enable pointer motion events for selection drag */
    t->canvas = IswCreateManagedWidget(name ? name : "term",
                                       drawingAreaWidgetClass,
                                       parent, ab.args, ab.count);

    IswAddCallback(t->canvas, IswNexposeCallback, expose_cb, t);
    IswAddCallback(t->canvas, IswNresizeCallback, resize_cb, t);
    IswAddCallback(t->canvas, IswNinputCallback,  input_cb,  t);

    recompute_metrics(t);

    /* Set preferred size */
    int px_w = (int)(t->cell_w * t->initial_cols) + 4;
    int px_h = (int)(t->cell_h * t->initial_rows) + 4;
    IswArgBuilderReset(&ab);
    IswArgWidth(&ab, px_w);
    IswArgHeight(&ab, px_h);
    IswSetValues(t->canvas, ab.args, ab.count);

    return t;
}

void term_widget_destroy(TermWidget *t)
{
    if (!t) return;
    if (t->vte) tsm_vte_unref(t->vte);
    if (t->screen) tsm_screen_unref(t->screen);
    if (t->font_face) cairo_font_face_destroy(t->font_face);
    if (t->key_syms) xcb_key_symbols_free(t->key_syms);
    free(t);
}

Widget term_widget_canvas(TermWidget *t) { return t->canvas; }

void term_widget_attach_pty(TermWidget *t, TermPty *pty)
{
    t->pty = pty;
}

void term_widget_feed(TermWidget *t, const char *buf, size_t n)
{
    tsm_vte_input(t->vte, buf, n);
    request_redraw(t);
}

void term_widget_invalidate(TermWidget *t) { request_redraw(t); }

void term_widget_apply_config(TermWidget *t, const TermConfig *cfg)
{
    t->cfg = *cfg;
    tsm_screen_set_max_sb(t->screen, cfg->scrollback);
    term_tsm_apply_palette(t->vte, &cfg->palette);
    recompute_metrics(t);
    /* Trigger a resize recompute with new cell metrics */
    Dimension pw, ph;
    IswArgBuilder ab = IswArgBuilderInit();
    IswArgWidth(&ab, &pw);
    IswArgHeight(&ab, &ph);
    IswGetValues(t->canvas, ab.args, ab.count);
    unsigned cols = t->cell_w > 0 ? (unsigned)(pw / t->cell_w) : t->cols;
    unsigned rows = t->cell_h > 0 ? (unsigned)(ph / t->cell_h) : t->rows;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    tsm_screen_resize(t->screen, cols, rows);
    t->cols = cols; t->rows = rows;
    if (t->pty) term_pty_resize(t->pty, cols, rows, pw, ph);
    request_redraw(t);
}

void term_widget_preferred_pixels(TermWidget *t, int cols, int rows,
                                  int *px_w, int *px_h)
{
    if (px_w) *px_w = (int)(t->cell_w * (cols > 0 ? cols : t->initial_cols)) + 4;
    if (px_h) *px_h = (int)(t->cell_h * (rows > 0 ? rows : t->initial_rows)) + 4;
}
