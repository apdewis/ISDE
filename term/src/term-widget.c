#define _POSIX_C_SOURCE 200809L
#include "term.h"

#include <ISW/Intrinsic.h>
#include <ISW/IntrinsicP.h>
#include <ISW/DrawingArea.h>
#include <ISW/StringDefs.h>
#include <ISW/IswArgMacros.h>
#include <ISW/IswDragDrop.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xcb.h>
#include <cairo/cairo-ft.h>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#define XK_MISCELLANY
#define XK_LATIN1
#define XK_XKB_KEYS
#include <X11/keysymdef.h>

#include <libtsm.h>

/* tsm_screen_draw_cb's cell-id parameter is uint32_t upstream, uint64_t in the
 * Aetf fork; CMake detects which and defines TSM_DRAW_ID_TYPE.  Default to the
 * fork's type if built without that detection. */
#ifndef TSM_DRAW_ID_TYPE
#define TSM_DRAW_ID_TYPE uint64_t
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward decls from term-tsm.c */
int  term_tsm_new(struct tsm_screen **s, struct tsm_vte **v,
                  tsm_vte_write_cb cb, void *user);
void term_tsm_apply_palette(struct tsm_vte *vte, const TermPalette *pal);

struct TermWidget;
static void ensure_atoms(TermWidget *t);
static void copy_selection_to(TermWidget *t, Atom which);
static void request_paste(TermWidget *t, Atom which);

static TermWidget *g_sel_owner;

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

    /* selection (cell coords) */
    int  sel_active;      /* a selection currently exists on screen */
    int  sel_pressed;     /* left button held down */
    int  sel_started;     /* tsm_screen_selection_start has been called this drag */
    int  press_cx;
    int  press_cy;

    /* clipboard */
    char *sel_text;       /* UTF-8 copy of current selection; owned, free() */
    size_t sel_len;
    IswTime last_event_time;

    /* interned selection-related atoms (lazy) */
    Atom a_primary;
    Atom a_clipboard;
    Atom a_utf8_string;
    Atom a_targets;
    Atom a_text;
    Atom a_timestamp;

    /* preferred initial geometry */
    int  initial_cols;
    int  initial_rows;

    /* redraw coalescing */
    Boolean         dirty;
    IswIntervalId   redraw_timer;

    /* cached render surface */
    cairo_surface_t *surf;
    unsigned char   *rgba_buf;
    int              surf_w;
    int              surf_h;

    /* previous cursor position for incremental redraw */
    unsigned        prev_cx;
    unsigned        prev_cy;
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
    unsigned    cur_cx, cur_cy;
    unsigned    prev_cx, prev_cy;
} DrawCtx;

static void rgb_unpack(const uint8_t in[3], double out[3])
{
    out[0] = in[0] / 255.0;
    out[1] = in[1] / 255.0;
    out[2] = in[2] / 255.0;
}

static int draw_cell_cb(struct tsm_screen *con, TSM_DRAW_ID_TYPE id,
                        const uint32_t *ch, size_t len, unsigned int width,
                        unsigned int posx, unsigned int posy,
                        const struct tsm_screen_attr *attr,
                        tsm_age_t age, void *data)
{
    (void)con; (void)id;
    DrawCtx *dc = (DrawCtx *)data;

    if (age != 0 && age <= dc->t->last_age &&
        !(posx == dc->cur_cx && posy == dc->cur_cy) &&
        !(posx == dc->prev_cx && posy == dc->prev_cy)) return 0;
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
    ISWRenderContext *rc = d->render_ctx;
    if (!rc) return;

    Dimension pw, ph;
    IswArgBuilder qb = IswArgBuilderInit();
    IswArgWidth(&qb, &pw);
    IswArgHeight(&qb, &ph);
    IswGetValues(t->canvas, qb.args, qb.count);
    if (pw <= 0 || ph <= 0) return;

    double sf = ISWScaleFactor(t->canvas);
    if (sf < 1.0) sf = 1.0;
    int width = (int)(pw * sf + 0.5);
    int height = (int)(ph * sf + 0.5);

    if (t->surf_w != width || t->surf_h != height) {
        if (t->surf) cairo_surface_destroy(t->surf);
        free(t->rgba_buf);
        t->surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                             width, height);
        t->rgba_buf = malloc((size_t)width * height * 4);
        t->surf_w = width;
        t->surf_h = height;
        t->last_age = 0;
    }
    if (cairo_surface_status(t->surf) != CAIRO_STATUS_SUCCESS) return;
    if (!t->rgba_buf) return;

    cairo_t *cr = cairo_create(t->surf);
    cairo_scale(cr, sf, sf);

    if (t->last_age == 0) {
        double bg[3];
        rgb_unpack(t->cfg.palette.rgb[17], bg);
        cairo_set_source_rgb(cr, bg[0], bg[1], bg[2]);
        cairo_paint(cr);
    }

    unsigned cx = tsm_screen_get_cursor_x(t->screen);
    unsigned cy = tsm_screen_get_cursor_y(t->screen);
    DrawCtx ctx = { t, cr, cx, cy, t->prev_cx, t->prev_cy };
    t->last_age = tsm_screen_draw(t->screen, draw_cell_cb, &ctx);
    draw_cursor(t, cr);
    t->prev_cx = cx;
    t->prev_cy = cy;

    cairo_destroy(cr);
    cairo_surface_flush(t->surf);

    const unsigned char *src = cairo_image_surface_get_data(t->surf);
    int stride = cairo_image_surface_get_stride(t->surf);
    for (int y = 0; y < height; y++) {
        const uint32_t *row = (const uint32_t *)(src + (size_t)y * stride);
        unsigned char *out = t->rgba_buf + (size_t)y * width * 4;
        for (int x = 0; x < width; x++) {
            uint32_t px = row[x];
            unsigned a = (px >> 24) & 0xFF;
            unsigned r = (px >> 16) & 0xFF;
            unsigned g = (px >> 8)  & 0xFF;
            unsigned b =  px        & 0xFF;
            if (a != 0 && a != 0xFF) {
                r = (r * 255 + a / 2) / a;
                g = (g * 255 + a / 2) / a;
                b = (b * 255 + a / 2) / a;
            }
            out[x * 4 + 0] = (unsigned char)r;
            out[x * 4 + 1] = (unsigned char)g;
            out[x * 4 + 2] = (unsigned char)b;
            out[x * 4 + 3] = (unsigned char)a;
        }
    }
    ISWRenderDrawImageRGBA(rc, t->rgba_buf,
                           (unsigned int)width, (unsigned int)height,
                           0, 0,
                           (unsigned int)pw, (unsigned int)ph);
}

static void redraw_timer_cb(IswPointer closure, IswIntervalId *id)
{
    (void)id;
    TermWidget *t = (TermWidget *)closure;
    t->redraw_timer = 0;
    t->dirty = False;
    if (t->pty) term_pty_resume(t->pty);
    IswExposeProc expose = IswClass(t->canvas)->core_class.expose;
    if (expose) {
        expose(t->canvas, NULL, 0);
    }
}

static void request_redraw(TermWidget *t)
{
    if (!IswIsRealized(t->canvas)) return;
    if (t->dirty) return;
    t->dirty = True;
    if (t->pty) term_pty_pause(t->pty);
    t->redraw_timer = IswAppAddTimeOut(
        IswWidgetToApplicationContext(t->canvas),
        16, redraw_timer_cb, (IswPointer)t);
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

static unsigned mods_to_tsm(uint16_t isw_mods)
{
    unsigned m = 0;
    if (isw_mods & IswModShift)   m |= TSM_SHIFT_MASK;
    if (isw_mods & IswModLock)    m |= TSM_LOCK_MASK;
    if (isw_mods & IswModControl) m |= TSM_CONTROL_MASK;
    if (isw_mods & IswModAlt)     m |= TSM_ALT_MASK;
    if (isw_mods & IswModSuper)   m |= TSM_LOGO_MASK;
    return m;
}

static uint32_t iswkey_to_xkeysym(uint32_t key)
{
    if (key < 0x110000) return key;
    switch (key) {
    case IswKeyBackspace:  return XK_BackSpace;
    case IswKeyTab:        return XK_Tab;
    case IswKeyReturn:     return XK_Return;
    case IswKeyEscape:     return XK_Escape;
    case IswKeyDelete:     return XK_Delete;
    case IswKeyHome:       return XK_Home;
    case IswKeyEnd:        return XK_End;
    case IswKeyArrowLeft:  return XK_Left;
    case IswKeyArrowRight: return XK_Right;
    case IswKeyArrowUp:    return XK_Up;
    case IswKeyArrowDown:  return XK_Down;
    case IswKeyPageUp:     return XK_Prior;
    case IswKeyPageDown:   return XK_Next;
    case IswKeyInsert:     return XK_Insert;
    case IswKeyF1:         return XK_F1;
    case IswKeyF2:         return XK_F2;
    case IswKeyF3:         return XK_F3;
    case IswKeyF4:         return XK_F4;
    case IswKeyF5:         return XK_F5;
    case IswKeyF6:         return XK_F6;
    case IswKeyF7:         return XK_F7;
    case IswKeyF8:         return XK_F8;
    case IswKeyF9:         return XK_F9;
    case IswKeyF10:        return XK_F10;
    case IswKeyF11:        return XK_F11;
    case IswKeyF12:        return XK_F12;
    case IswKeyShift:      return XK_Shift_L;
    case IswKeyControl:    return XK_Control_L;
    case IswKeyAlt:        return XK_Alt_L;
    case IswKeySuper:      return XK_Super_L;
    case IswKeyMeta:       return XK_Meta_L;
    case IswKeyCapsLock:   return XK_Caps_Lock;
    case IswKeyNumLock:    return XK_Num_Lock;
    case IswKeyMenu:       return XK_Menu;
    case IswKeyPause:      return XK_Pause;
    case IswKeyPrint:      return XK_Print;
    default:               return XK_VoidSymbol;
    }
}

static void handle_key_press(TermWidget *t, IswKeyEvent *kev)
{
    uint32_t sym = iswkey_to_xkeysym(kev->key);
    if (sym == XK_VoidSymbol) return;
    if (sym >= XK_Shift_L && sym <= XK_Hyper_R) return;
    if (sym == XK_Num_Lock) return;

    uint32_t uc = kev->unicode;
    uint16_t mods_mask = kev->modifiers;

    /* Local hotkeys */
    if ((mods_mask & IswModShift) && kev->key == IswKeyPageUp) {
        tsm_screen_sb_page_up(t->screen, 1);
        request_redraw(t);
        return;
    }
    if ((mods_mask & IswModShift) && kev->key == IswKeyPageDown) {
        tsm_screen_sb_page_down(t->screen, 1);
        request_redraw(t);
        return;
    }
    {
        bool ctrl  = (mods_mask & IswModControl) != 0;
        bool shift = (mods_mask & IswModShift) != 0;
        if (ctrl && shift && (uc == 'C' || uc == 'c')) {
            ensure_atoms(t);
            copy_selection_to(t, t->a_clipboard);
            return;
        }
        if (ctrl && shift && (uc == 'V' || uc == 'v')) {
            ensure_atoms(t);
            request_paste(t, t->a_clipboard);
            return;
        }
        if (shift && kev->key == IswKeyInsert) {
            request_paste(t, t->a_primary);
            return;
        }
    }

    unsigned mods = mods_to_tsm(mods_mask);

    bool consumed = tsm_vte_handle_keyboard(t->vte, sym,
                                            uc ? uc : 0,
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

/* ---------- clipboard (Xt selection API) ---------- */

static void ensure_atoms(TermWidget *t)
{
    if (t->a_primary) return;
    Widget w = _IswWidgetAncestor(t->canvas);
    t->a_primary     = IswDndInternType(w, "PRIMARY");
    t->a_clipboard   = IswDndInternType(w, "CLIPBOARD");
    t->a_utf8_string = IswDndInternType(w, "UTF8_STRING");
    t->a_targets     = IswDndInternType(w, "TARGETS");
    t->a_text        = IswDndInternType(w, "TEXT");
    t->a_timestamp   = IswDndInternType(w, "TIMESTAMP");
}

static Boolean convert_selection(Widget w, Atom *selection, Atom *target,
                                 Atom *type_return, IswPointer *value_return,
                                 unsigned long *length_return, int *format_return)
{
    (void)selection;
    (void)w;
    TermWidget *t = g_sel_owner;
    if (!t || !t->sel_text || !t->sel_len)
        return False;

    if (*target == t->a_targets) {
        static Atom targets[3];
        targets[0] = t->a_targets;
        targets[1] = t->a_utf8_string;
        targets[2] = t->a_text;
        *type_return = t->a_targets;
        *value_return = (IswPointer)targets;
        *length_return = 3;
        *format_return = 32;
        return True;
    }

    if (*target == t->a_utf8_string || *target == t->a_text) {
        *type_return = t->a_utf8_string;
        *value_return = (IswPointer)t->sel_text;
        *length_return = t->sel_len;
        *format_return = 8;
        return True;
    }

    return False;
}

static void lose_selection(Widget w, Atom *selection)
{
    (void)w; (void)selection;
}

static void deliver_paste_bytes(TermWidget *t, const unsigned char *data,
                                unsigned long len)
{
    char *buf = malloc(len + 1);
    if (!buf) return;
    size_t out = 0;
    for (unsigned long i = 0; i < len; i++) {
        unsigned char ch = data[i];
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

static void receive_paste(Widget w, IswPointer client_data,
                          Atom *selection, Atom *type,
                          IswPointer value, unsigned long *length,
                          int *format)
{
    (void)w; (void)selection; (void)format;
    TermWidget *t = (TermWidget *)client_data;
    if (!value || *length == 0 || *type == None)
        return;
    deliver_paste_bytes(t, (const unsigned char *)value, *length);
}

static void copy_selection_to(TermWidget *t, Atom which)
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
    g_sel_owner = t;
    Widget shell = _IswWidgetAncestor(t->canvas);
    IswOwnSelection(shell, (Atom)which, t->last_event_time,
                    convert_selection, lose_selection, NULL);
}

static void request_paste(TermWidget *t, Atom which)
{
    ensure_atoms(t);
    if (t->sel_active) {
        tsm_screen_selection_reset(t->screen);
        t->sel_active = 0;
        request_redraw(t);
    }
    Widget shell = _IswWidgetAncestor(t->canvas);
    IswGetSelectionValue(shell, (Atom)which, t->a_utf8_string,
                        receive_paste, t, 0);
}

static void handle_button(TermWidget *t, IswButtonEvent *bev, bool press)
{
    int cx = (int)(bev->x / t->cell_w);
    int cy = (int)(bev->y / t->cell_h);
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;

    if (bev->button == IswButtonWheelUp) {
        if (!press) return;
        tsm_screen_sb_up(t->screen, 3);
        request_redraw(t);
        return;
    }
    if (bev->button == IswButtonWheelDown) {
        if (!press) return;
        tsm_screen_sb_down(t->screen, 3);
        request_redraw(t);
        return;
    }

    if (bev->button == IswButtonLeft) {
        if (press) {
            t->sel_pressed = 1;
            t->sel_started = 0;
            t->press_cx = cx;
            t->press_cy = cy;
        } else {
            t->sel_pressed = 0;
            if (!t->sel_started && t->sel_active) {
                tsm_screen_selection_reset(t->screen);
                t->sel_active = 0;
                request_redraw(t);
            } else if (t->sel_started && t->sel_active) {
                copy_selection_to(t, t->a_primary);
            }
        }
        return;
    }

    if (bev->button == IswButtonMiddle) {
        if (press) request_paste(t, t->a_primary);
        return;
    }
}

static void handle_motion(TermWidget *t, IswMotionEvent *mev)
{
    if (!t->sel_pressed) return;
    int cx = (int)(mev->x / t->cell_w);
    int cy = (int)(mev->y / t->cell_h);
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
    IswEvent *ev = d->event;
    if (!ev) return;
    switch (ev->kind) {
    case IswKeyDown:
        t->last_event_time = ev->any.time;
        handle_key_press(t, &ev->key);
        break;
    case IswButtonDown:
        t->last_event_time = ev->any.time;
        handle_button(t, &ev->button, true);
        break;
    case IswButtonUp:
        t->last_event_time = ev->any.time;
        handle_button(t, &ev->button, false);
        break;
    case IswMotion:
        handle_motion(t, &ev->motion);
        break;
    default: break;
    }
}

/* ---------- OSC (Operating System Command) ---------- */

#ifdef HAVE_TSM_OSC_CB
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
#endif /* HAVE_TSM_OSC_CB */

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
#ifdef HAVE_TSM_OSC_CB
    tsm_vte_set_osc_cb(t->vte, osc_cb, t);
#endif
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
    if (t->redraw_timer) IswRemoveTimeOut(t->redraw_timer);
    if (t->surf) cairo_surface_destroy(t->surf);
    free(t->rgba_buf);
    if (t->vte) tsm_vte_unref(t->vte);
    if (t->screen) tsm_screen_unref(t->screen);
    term_fonts_clear(t);
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

void term_widget_invalidate(TermWidget *t) { t->last_age = 0; request_redraw(t); }

void term_widget_apply_config(TermWidget *t, const TermConfig *cfg)
{
    t->cfg = *cfg;
    tsm_screen_set_max_sb(t->screen, cfg->scrollback);
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
    t->last_age = 0;
    request_redraw(t);
}

void term_widget_preferred_pixels(TermWidget *t, int cols, int rows,
                                  int *px_w, int *px_h)
{
    if (px_w) *px_w = (int)(t->cell_w * (cols > 0 ? cols : t->initial_cols)) + 4;
    if (px_h) *px_h = (int)(t->cell_h * (rows > 0 ? rows : t->initial_rows)) + 4;
}
