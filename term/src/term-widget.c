#define _POSIX_C_SOURCE 200809L
#include "term.h"

#include <ISW/Intrinsic.h>
#include <ISW/IntrinsicP.h>
#include <ISW/DrawingArea.h>
#include <ISW/StringDefs.h>
#include <ISW/IswArgMacros.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xcb.h>
#include <cairo/cairo-ft.h>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

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
static void ensure_atoms(TermWidget *t);
static void copy_selection_to(TermWidget *t, xcb_atom_t which);
static void request_paste(TermWidget *t, xcb_atom_t which);
static void ensure_selection_handler(TermWidget *t);

struct TermWidget {
    Widget      canvas;
    TermConfig  cfg;

    struct tsm_screen *screen;
    struct tsm_vte    *vte;
    TermPty           *pty;

    /* Font rendering: primary FreeType/FcCharSet-backed cairo face plus a
     * small cache of fallback faces keyed by codepoint coverage. */
    double             font_size_px;
    double             cell_w;
    double             cell_h;
    double             baseline;

    struct TermFont   *primary_font;
    struct TermFont  **fallback_fonts;  /* dyn array */
    size_t             fallback_count;
    size_t             fallback_cap;

    unsigned           cols;
    unsigned           rows;
    tsm_age_t          last_age;
    int                bg_suppressed;

    xcb_key_symbols_t *key_syms;

    /* selection (cell coords) */
    int  sel_active;      /* a selection currently exists on screen */
    int  sel_pressed;     /* left button held down */
    int  sel_started;     /* tsm_screen_selection_start has been called this drag */
    int  press_cx;
    int  press_cy;

    /* clipboard */
    char *sel_text;       /* UTF-8 copy of current selection; owned, free() */
    size_t sel_len;
    xcb_timestamp_t last_event_time; /* timestamp from most recent input event */

    /* interned selection-related atoms (lazy) */
    xcb_atom_t a_primary;
    xcb_atom_t a_clipboard;
    xcb_atom_t a_utf8_string;
    xcb_atom_t a_targets;
    xcb_atom_t a_text;
    xcb_atom_t a_timestamp;
    xcb_atom_t a_paste_prop;
    xcb_timestamp_t sel_own_time; /* timestamp we claimed selection at */
    int paste_fallback_used;

    /* preferred initial geometry */
    int  initial_cols;
    int  initial_rows;
};

/* ---------- rendering ---------- */

typedef struct TermFont {
    FT_Face            ft_face;
    cairo_font_face_t *cr_face;
    FcCharSet         *charset;  /* owned */
    char              *file;     /* owned, for dedup */
} TermFont;

static FT_Library g_ft_lib;

static void term_ft_init(void)
{
    if (!g_ft_lib) FT_Init_FreeType(&g_ft_lib);
}

static TermFont *term_font_from_pattern(FcPattern *match)
{
    FcChar8 *file = NULL;
    if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch)
        return NULL;
    int index = 0;
    FcPatternGetInteger(match, FC_INDEX, 0, &index);

    FT_Face ft = NULL;
    if (FT_New_Face(g_ft_lib, (const char *)file, index, &ft) != 0)
        return NULL;

    cairo_font_face_t *cr = cairo_ft_font_face_create_for_ft_face(ft, 0);
    if (!cr || cairo_font_face_status(cr) != CAIRO_STATUS_SUCCESS) {
        if (cr) cairo_font_face_destroy(cr);
        FT_Done_Face(ft);
        return NULL;
    }

    TermFont *f = calloc(1, sizeof(*f));
    f->ft_face = ft;
    f->cr_face = cr;
    f->file    = strdup((const char *)file);

    FcCharSet *cs = NULL;
    if (FcPatternGetCharSet(match, FC_CHARSET, 0, &cs) == FcResultMatch && cs)
        f->charset = FcCharSetCopy(cs);

    return f;
}

static void term_font_free(TermFont *f)
{
    if (!f) return;
    if (f->cr_face) cairo_font_face_destroy(f->cr_face);
    if (f->ft_face) FT_Done_Face(f->ft_face);
    if (f->charset) FcCharSetDestroy(f->charset);
    free(f->file);
    free(f);
}

static TermFont *term_resolve_primary(const char *family)
{
    term_ft_init();
    FcPattern *pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)(family ? family : "Monospace"));
    FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pat, &result);
    FcPatternDestroy(pat);
    if (!match) return NULL;
    TermFont *f = term_font_from_pattern(match);
    FcPatternDestroy(match);
    return f;
}

static TermFont *term_resolve_fallback_for(TermWidget *t, uint32_t ucs)
{
    /* Cached? */
    for (size_t i = 0; i < t->fallback_count; i++) {
        TermFont *f = t->fallback_fonts[i];
        if (f->charset && FcCharSetHasChar(f->charset, ucs))
            return f;
    }

    term_ft_init();
    FcCharSet *cs = FcCharSetCreate();
    FcCharSetAddChar(cs, ucs);
    FcPattern *pat = FcPatternCreate();
    FcPatternAddCharSet(pat, FC_CHARSET, cs);
    FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pat, &result);
    FcCharSetDestroy(cs);
    FcPatternDestroy(pat);
    if (!match) return NULL;

    TermFont *f = term_font_from_pattern(match);
    FcPatternDestroy(match);
    if (!f) return NULL;

    /* Dedup by file */
    for (size_t i = 0; i < t->fallback_count; i++) {
        if (t->fallback_fonts[i]->file && f->file &&
            strcmp(t->fallback_fonts[i]->file, f->file) == 0) {
            term_font_free(f);
            return t->fallback_fonts[i];
        }
    }

    if (t->fallback_count == t->fallback_cap) {
        size_t nc = t->fallback_cap ? t->fallback_cap * 2 : 8;
        t->fallback_fonts = realloc(t->fallback_fonts, nc * sizeof(*t->fallback_fonts));
        t->fallback_cap = nc;
    }
    t->fallback_fonts[t->fallback_count++] = f;
    return f;
}

static TermFont *term_font_for_codepoint(TermWidget *t, uint32_t ucs)
{
    if (t->primary_font && t->primary_font->charset &&
        FcCharSetHasChar(t->primary_font->charset, ucs))
        return t->primary_font;
    if (!t->primary_font) return NULL;
    TermFont *fb = term_resolve_fallback_for(t, ucs);
    return fb ? fb : t->primary_font;
}

static void term_fonts_clear(TermWidget *t)
{
    if (t->primary_font) { term_font_free(t->primary_font); t->primary_font = NULL; }
    for (size_t i = 0; i < t->fallback_count; i++) term_font_free(t->fallback_fonts[i]);
    free(t->fallback_fonts);
    t->fallback_fonts = NULL;
    t->fallback_count = t->fallback_cap = 0;
}

static void recompute_metrics(TermWidget *t)
{
    t->font_size_px = (double)t->cfg.font_size * 96.0 / 72.0;

    term_fonts_clear(t);
    t->primary_font = term_resolve_primary(t->cfg.font_family);

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 16, 16);
    cairo_t *cr = cairo_create(surf);
    if (t->primary_font)
        cairo_set_font_face(cr, t->primary_font->cr_face);
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
    {
        bool is_cursor = (posx == tsm_screen_get_cursor_x(t->screen) &&
                          posy == tsm_screen_get_cursor_y(t->screen));
        bool do_inverse = attr->inverse &&
                          !(is_cursor && strcmp(t->cfg.cursor_shape, "block") != 0);
        if (do_inverse) {
            uint8_t tr=fr,tg=fg,tb=fb;
            fr=br; fg=bg_; fb=bb;
            br=tr; bg_=tg; bb=tb;
        }
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

    /* Render each codepoint with a font that actually covers it. Combining
     * marks (len > 1) typically share coverage with the base, but resolve
     * per-codepoint anyway so odd combinations still render. */
    cairo_set_font_size(cr, t->font_size_px);
    cairo_set_source_rgb(cr, fr/255.0, fg/255.0, fb/255.0);
    cairo_move_to(cr, x, y + t->baseline);

    for (size_t k = 0; k < len; k++) {
        uint32_t c = ch[k];
        if (!c) continue;

        char utf8[8];
        int n = 0;
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
        utf8[n] = '\0';

        TermFont *font = term_font_for_codepoint(t, c);
        if (font) cairo_set_font_face(cr, font->cr_face);
        cairo_show_text(cr, utf8);
    }

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

    /* Local hotkeys: Shift+PgUp/PgDn, Ctrl+Shift+C/V, Shift+Insert */
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
    {
        bool ctrl  = (kev->state & XCB_MOD_MASK_CONTROL) != 0;
        bool shift = (kev->state & XCB_MOD_MASK_SHIFT) != 0;
        if (ctrl && shift && (sym == XK_C || sym == XK_c)) {
            ensure_atoms(t);
            copy_selection_to(t, t->a_clipboard);
            return;
        }
        if (ctrl && shift && (sym == XK_V || sym == XK_v)) {
            ensure_atoms(t);
            request_paste(t, t->a_clipboard);
            return;
        }
        if (shift && sym == XK_Insert) {
            request_paste(t, XCB_ATOM_PRIMARY);
            return;
        }
    }

    unsigned mods = mods_to_tsm(kev->state);
    uint32_t uc = keysym_to_unicode(sym);

    bool consumed = tsm_vte_handle_keyboard(t->vte, sym,
                                            uc == TSM_VTE_INVALID ? 0 : uc,
                                            mods, uc);
    if (consumed) {
        tsm_screen_sb_reset(t->screen);
        if (t->sel_active) {
            tsm_screen_selection_reset(t->screen);
            t->sel_active = 0;
        }
        request_redraw(t);
    }
}

/* ---------- clipboard ---------- */

/* Pure-XCB clipboard: own the selection with xcb_set_selection_owner, answer
 * SelectionRequest events ourselves, and issue paste via xcb_convert_selection.
 * No libISW selection API, no Xt dispatch subtleties. */

static xcb_atom_t intern(xcb_connection_t *c, const char *name)
{
    xcb_intern_atom_cookie_t ck = xcb_intern_atom(c, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, ck, NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

static void ensure_atoms(TermWidget *t)
{
    if (t->a_primary) return;
    xcb_connection_t *c = IswDisplay(t->canvas);
    t->a_primary     = XCB_ATOM_PRIMARY;
    t->a_clipboard   = intern(c, "CLIPBOARD");
    t->a_utf8_string = intern(c, "UTF8_STRING");
    t->a_targets     = intern(c, "TARGETS");
    t->a_text        = intern(c, "TEXT");
    t->a_timestamp   = intern(c, "TIMESTAMP");
    t->a_paste_prop  = intern(c, "ISDE_TERM_PASTE");
}

static void send_selection_notify(xcb_connection_t *c,
                                  xcb_selection_request_event_t *req,
                                  xcb_atom_t property)
{
    xcb_selection_notify_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.response_type = XCB_SELECTION_NOTIFY;
    ev.time      = req->time;
    ev.requestor = req->requestor;
    ev.selection = req->selection;
    ev.target    = req->target;
    ev.property  = property;
    xcb_send_event(c, 0, req->requestor, 0, (const char *)&ev);
}

static void handle_selection_request(TermWidget *t,
                                     xcb_selection_request_event_t *req)
{
    xcb_connection_t *c = IswDisplay(t->canvas);
    xcb_atom_t property = req->property != XCB_ATOM_NONE ? req->property : req->target;

    if (!t->sel_text || !t->sel_len) {
        send_selection_notify(c, req, XCB_ATOM_NONE);
        xcb_flush(c);
        return;
    }

    if (req->target == t->a_targets) {
        xcb_atom_t list[5] = { t->a_targets, t->a_timestamp,
                               t->a_utf8_string, XCB_ATOM_STRING, t->a_text };
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, req->requestor, property,
                            XCB_ATOM_ATOM, 32, 5, list);
        send_selection_notify(c, req, property);
    } else if (req->target == t->a_timestamp) {
        uint32_t ts = t->sel_own_time;
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, req->requestor, property,
                            XCB_ATOM_INTEGER, 32, 1, &ts);
        send_selection_notify(c, req, property);
    } else if (req->target == XCB_ATOM_STRING || req->target == t->a_text) {
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, req->requestor, property,
                            XCB_ATOM_STRING, 8, t->sel_len, t->sel_text);
        send_selection_notify(c, req, property);
    } else if (req->target == t->a_utf8_string) {
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, req->requestor, property,
                            t->a_utf8_string, 8, t->sel_len, t->sel_text);
        send_selection_notify(c, req, property);
    } else {
        send_selection_notify(c, req, XCB_ATOM_NONE);
    }
    xcb_flush(c);
}

static void deliver_paste_bytes(TermWidget *t, const unsigned char *data,
                                unsigned long len)
{
    char *buf = malloc(len + 1);
    if (!buf) return;
    size_t out = 0;
    for (unsigned long i = 0; i < len; i++) {
        unsigned char ch = data[i];
        /* Drop C0 controls except tab/newline/carriage-return and DEL.
         * Pass through all bytes >= 0x80 so UTF-8 sequences survive intact. */
        if (ch < 0x20) {
            if (ch != '\t' && ch != '\n' && ch != '\r') continue;
        } else if (ch == 0x7F) {
            continue;
        }
        buf[out++] = (char)ch;
    }
    buf[out] = '\0';
    if (out && t->pty) term_pty_write(t->pty, buf, out);
    free(buf);
}

static void handle_selection_notify(TermWidget *t,
                                    xcb_selection_notify_event_t *e)
{
    if (e->requestor != IswWindow(t->canvas)) return;
    if (e->property == XCB_ATOM_NONE) {
        /* Owner refused. Fall back once from UTF8_STRING → STRING. */
        if (e->target == t->a_utf8_string && !t->paste_fallback_used) {
            t->paste_fallback_used = 1;
            xcb_convert_selection(IswDisplay(t->canvas),
                                  IswWindow(t->canvas),
                                  e->selection, XCB_ATOM_STRING,
                                  t->a_paste_prop, XCB_CURRENT_TIME);
            xcb_flush(IswDisplay(t->canvas));
        }
        return;
    }

    xcb_connection_t *c = IswDisplay(t->canvas);
    xcb_window_t win = IswWindow(t->canvas);
    xcb_get_property_cookie_t ck = xcb_get_property(c, 1, win, e->property,
                                                    XCB_GET_PROPERTY_TYPE_ANY,
                                                    0, UINT32_MAX / 4);
    xcb_get_property_reply_t *r = xcb_get_property_reply(c, ck, NULL);
    if (r) {
        unsigned long len = xcb_get_property_value_length(r);
        if (len > 0 && r->format == 8) {
            deliver_paste_bytes(t, xcb_get_property_value(r), len);
        }
        free(r);
    }
    xcb_delete_property(c, win, e->property);
    xcb_flush(c);
}

static void selection_event_handler(Widget w, IswPointer closure,
                                    xcb_generic_event_t *event, Boolean *cont)
{
    (void)w; (void)cont;
    TermWidget *t = (TermWidget *)closure;
    if (!event) return;
    uint8_t type = event->response_type & ~0x80;
    switch (type) {
    case XCB_SELECTION_REQUEST:
        handle_selection_request(t, (xcb_selection_request_event_t *)event);
        break;
    case XCB_SELECTION_NOTIFY:
        handle_selection_notify(t, (xcb_selection_notify_event_t *)event);
        break;
    case XCB_SELECTION_CLEAR:
        /* We lost ownership. Nothing to do; keep sel_text visible on screen. */
        break;
    }
}

static int selection_handler_installed = 0;
static void ensure_selection_handler(TermWidget *t)
{
    if (selection_handler_installed) return;
    selection_handler_installed = 1;
    IswAddEventHandler(t->canvas, (EventMask)0, True,
                       selection_event_handler, t);
}

static void copy_selection_to(TermWidget *t, xcb_atom_t which)
{
    if (!t->sel_active || !t->screen) return;
    char *out = NULL;
    int n = tsm_screen_selection_copy(t->screen, &out);
    if (n < 0 || !out) { free(out); return; }

    size_t len = (size_t)n;
    while (len > 0 && out[len - 1] == '\0') len--;
    for (size_t i = 0; i < len; i++) {
        if (out[i] == '\0') out[i] = ' ';
    }
    free(t->sel_text);
    t->sel_text = out;
    t->sel_len  = len;

    ensure_atoms(t);
    ensure_selection_handler(t);
    t->sel_own_time = t->last_event_time;
    xcb_connection_t *c = IswDisplay(t->canvas);
    xcb_set_selection_owner(c, IswWindow(t->canvas), which, t->last_event_time);
    xcb_flush(c);
}

static void request_paste(TermWidget *t, xcb_atom_t which)
{
    ensure_atoms(t);
    ensure_selection_handler(t);
    t->paste_fallback_used = 0;
    if (t->sel_active) {
        tsm_screen_selection_reset(t->screen);
        t->sel_active = 0;
        request_redraw(t);
    }
    xcb_connection_t *c = IswDisplay(t->canvas);
    xcb_convert_selection(c, IswWindow(t->canvas), which, t->a_utf8_string,
                          t->a_paste_prop, XCB_CURRENT_TIME);
    xcb_flush(c);
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
            t->sel_pressed = 1;
            t->sel_started = 0;
            t->press_cx = cx;
            t->press_cy = cy;
        } else {
            t->sel_pressed = 0;
            if (!t->sel_started && t->sel_active) {
                /* Simple click with no drag: clear any existing selection. */
                tsm_screen_selection_reset(t->screen);
                t->sel_active = 0;
                request_redraw(t);
            } else if (t->sel_started && t->sel_active) {
                /* Drag finalized: publish to PRIMARY. */
                copy_selection_to(t, XCB_ATOM_PRIMARY);
            }
        }
        return;
    }

    if (bev->detail == 2 /* middle — paste PRIMARY */) {
        if (press) request_paste(t, XCB_ATOM_PRIMARY);
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
    if (!t->sel_started) {
        if (cx == t->press_cx && cy == t->press_cy) return;
        t->sel_started = 1;
        t->sel_active  = 1;
    }
    tsm_screen_selection_start(t->screen, t->press_cx, t->press_cy);
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
    case XCB_KEY_PRESS: {
        xcb_key_press_event_t *e = (xcb_key_press_event_t *)d->event;
        t->last_event_time = e->time;
        handle_key_press(t, e);
        break;
    }
    case XCB_BUTTON_PRESS: {
        xcb_button_press_event_t *e = (xcb_button_press_event_t *)d->event;
        t->last_event_time = e->time;
        handle_button(t, e, true);
        break;
    }
    case XCB_BUTTON_RELEASE: {
        xcb_button_press_event_t *e = (xcb_button_press_event_t *)d->event;
        t->last_event_time = e->time;
        handle_button(t, e, false);
        break;
    }
    case XCB_MOTION_NOTIFY:
        handle_motion(t, (xcb_motion_notify_event_t *)d->event);
        break;
    default: break;
    }
}

/* ---------- OSC (Operating System Command) ---------- */

static Widget find_shell_ancestor(Widget w)
{
    while (w && !IswIsShell(w))
        w = IswParent(w);
    return w;
}

static void osc_cb(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
    (void)vte;
    TermWidget *t = (TermWidget *)data;
    if (!u8 || len < 2) return;

    /* OSC payload is "<code>;<string>".  We handle 0 (title+icon) and 2 (title). */
    const char *semi = memchr(u8, ';', len);
    if (!semi) return;
    int code = 0;
    for (const char *p = u8; p < semi; p++) {
        if (*p < '0' || *p > '9') return;
        code = code * 10 + (*p - '0');
    }
    if (code != 0 && code != 2) return;

    size_t title_len = len - (size_t)(semi + 1 - u8);
    char *title = malloc(title_len + 1);
    if (!title) return;
    memcpy(title, semi + 1, title_len);
    title[title_len] = '\0';

    Widget shell = find_shell_ancestor(t->canvas);
    if (shell) {
        IswArgBuilder ab = IswArgBuilderInit();
        IswArgTitle(&ab, title);
        if (code == 0)
            IswArgIconName(&ab, title);
        IswSetValues(shell, ab.args, ab.count);
    }
    free(title);
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
    tsm_vte_set_osc_cb(t->vte, osc_cb, t);
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
    term_fonts_clear(t);
    if (t->key_syms) xcb_key_symbols_free(t->key_syms);
    free(t->sel_text);
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
