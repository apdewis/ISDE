#define _POSIX_C_SOURCE 200809L

#include "compositor.h"
#include "wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/shape.h>
#include <xcb/xcb_aux.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

#include <cairo/cairo.h>
#include "render.h"
#include "isde/isde-theme.h"

/* EGL / GL extension function pointers (zero-copy path) */
static PFNEGLCREATEIMAGEKHRPROC   pfn_eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC  pfn_eglDestroyImageKHR;

typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum target, void *image);
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pfn_glEGLImageTargetTexture2DOES;

static int egl_image_available;

/* Framebuffer-object entry points (not in GL 1.1 headers) — used to copy a
 * window's live texture into an owned snapshot so fade-out never samples the
 * X pixmap storage that the server frees on unmap. */
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER          0x8D40
#define GL_COLOR_ATTACHMENT0    0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif

typedef void   (*PFNGLGENFRAMEBUFFERS)(GLsizei n, GLuint *ids);
typedef void   (*PFNGLBINDFRAMEBUFFER)(GLenum target, GLuint fb);
typedef void   (*PFNGLFRAMEBUFFERTEXTURE2D)(GLenum target, GLenum att,
                                            GLenum textarget, GLuint tex,
                                            GLint level);
typedef void   (*PFNGLDELETEFRAMEBUFFERS)(GLsizei n, const GLuint *ids);
typedef GLenum (*PFNGLCHECKFRAMEBUFFERSTATUS)(GLenum target);

static PFNGLGENFRAMEBUFFERS         pfn_glGenFramebuffers;
static PFNGLBINDFRAMEBUFFER         pfn_glBindFramebuffer;
static PFNGLFRAMEBUFFERTEXTURE2D    pfn_glFramebufferTexture2D;
static PFNGLDELETEFRAMEBUFFERS      pfn_glDeleteFramebuffers;
static PFNGLCHECKFRAMEBUFFERSTATUS  pfn_glCheckFramebufferStatus;

static int fbo_available;

static void load_extensions(void)
{
    pfn_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
        eglGetProcAddress("eglCreateImageKHR");
    pfn_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)
        eglGetProcAddress("eglDestroyImageKHR");
    pfn_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");

    egl_image_available = (pfn_eglCreateImageKHR &&
                           pfn_eglDestroyImageKHR &&
                           pfn_glEGLImageTargetTexture2DOES);

    if (egl_image_available) {
        fprintf(stderr, "compositor: using EGLImage zero-copy path\n");
    } else {
        fprintf(stderr, "compositor: EGLImage not available, "
                        "using glTexImage2D readback path\n");
    }

    pfn_glGenFramebuffers = (PFNGLGENFRAMEBUFFERS)
        eglGetProcAddress("glGenFramebuffers");
    pfn_glBindFramebuffer = (PFNGLBINDFRAMEBUFFER)
        eglGetProcAddress("glBindFramebuffer");
    pfn_glFramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2D)
        eglGetProcAddress("glFramebufferTexture2D");
    pfn_glDeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERS)
        eglGetProcAddress("glDeleteFramebuffers");
    pfn_glCheckFramebufferStatus = (PFNGLCHECKFRAMEBUFFERSTATUS)
        eglGetProcAddress("glCheckFramebufferStatus");

    fbo_available = (pfn_glGenFramebuffers && pfn_glBindFramebuffer &&
                     pfn_glFramebufferTexture2D && pfn_glDeleteFramebuffers &&
                     pfn_glCheckFramebufferStatus);

    if (!fbo_available) {
        fprintf(stderr, "compositor: FBO unavailable, fade-out disabled\n");
    }
}

/* ---------- window tracking ---------- */

static CompositorWindow *find_comp_window(WmCompositor *comp, xcb_window_t win)
{
    for (CompositorWindow *cw = comp->windows; cw; cw = cw->next) {
        if (cw->window == win) {
            return cw;
        }
    }
    return NULL;
}

/* Rebuild the window list to match the X server's stacking order.
 * QueryTree returns root's children bottom-to-top; we reuse the existing
 * nodes (preserving pixmaps/textures/damage) and reorder so the head is the
 * topmost window.  Filtering through find_comp_window drops untracked siblings
 * such as the composite overlay, which can appear as a ConfigureNotify's
 * above_sibling but must never enter the paint list. */
static void compositor_restack_from_tree(WmCompositor *comp)
{
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(comp->conn,
        xcb_query_tree(comp->conn, comp->screen->root), NULL);
    if (!tree) {
        return;
    }

    xcb_window_t *children = xcb_query_tree_children(tree);
    int n = xcb_query_tree_children_length(tree);

    CompositorWindow *newlist = NULL;
    for (int i = 0; i < n; i++) {
        CompositorWindow **pp = &comp->windows;
        while (*pp && (*pp)->window != children[i]) {
            pp = &(*pp)->next;
        }
        if (!*pp) {
            continue;
        }
        CompositorWindow *cw = *pp;
        *pp = cw->next;
        /* Refresh the cached above_sibling to the real lower neighbour from the
         * tree (children run bottom-to-top, so children[i-1] sits directly
         * below children[i]).  Without this the cache only ever updates for the
         * window that received a ConfigureNotify, so it goes stale after a
         * tree-driven reorder and the dedupe in wm_compositor_window_configured
         * can falsely skip a needed rebuild, painting a window in a stale
         * stacking position permanently. */
        cw->above = (i > 0) ? children[i - 1] : XCB_NONE;
        /* Prepend while walking bottom-to-top, so the topmost child ends up
         * at the head. */
        cw->next = newlist;
        newlist = cw;
    }

    /* Anything left untracked by QueryTree (should not happen) goes to the
     * bottom so no window is silently dropped. */
    if (comp->windows) {
        if (!newlist) {
            newlist = comp->windows;
        } else {
            CompositorWindow *t = newlist;
            while (t->next) {
                t = t->next;
            }
            t->next = comp->windows;
        }
    }

    comp->windows = newlist;
    free(tree);
}

static void ensure_texture(CompositorWindow *cw)
{
    if (!cw->texture) {
        glGenTextures(1, &cw->texture);
        glBindTexture(GL_TEXTURE_2D, cw->texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

static int bind_pixmap_eglimage(WmCompositor *comp, CompositorWindow *cw)
{
    if (!cw->pixmap) {
        return -1;
    }

    EGLint img_attrs[] = { EGL_NONE };
    EGLImageKHR image = pfn_eglCreateImageKHR(
        comp->egl_display, EGL_NO_CONTEXT,
        EGL_NATIVE_PIXMAP_KHR,
        (EGLClientBuffer)(uintptr_t)cw->pixmap,
        img_attrs);

    if (image == EGL_NO_IMAGE_KHR) {
        return -1;
    }

    ensure_texture(cw);
    glBindTexture(GL_TEXTURE_2D, cw->texture);
    pfn_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    glBindTexture(GL_TEXTURE_2D, 0);

    pfn_eglDestroyImageKHR(comp->egl_display, image);
    return 0;
}

static int bind_pixmap_readback(WmCompositor *comp, CompositorWindow *cw)
{
    if (!cw->pixmap || cw->pw == 0 || cw->ph == 0) {
        return -1;
    }

    xcb_get_image_reply_t *img = xcb_get_image_reply(comp->conn,
        xcb_get_image(comp->conn, XCB_IMAGE_FORMAT_Z_PIXMAP,
                      cw->pixmap, 0, 0, cw->pw, cw->ph,
                      0xFFFFFFFF), NULL);
    if (!img) {
        return -1;
    }

    uint8_t *data = xcb_get_image_data(img);
    int len = xcb_get_image_data_length(img);
    int npixels = cw->pw * cw->ph;
    if (len < npixels * 4) {
        free(img);
        return -1;
    }

    /* X11 24-bit visuals store BGRX with undefined alpha byte.
     * Force alpha to 0xFF so blending works correctly. */
    uint32_t *pixels = (uint32_t *)data;
    for (int i = 0; i < npixels; i++) {
        pixels[i] |= 0xFF000000;
    }

    ensure_texture(cw);
    glBindTexture(GL_TEXTURE_2D, cw->texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 cw->pw, cw->ph, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);

    free(img);
    return 0;
}

/* Copy the live texture into the window's owned snapshot texture via an FBO.
 * The on-screen paint draws the snapshot, so a window can keep fading out
 * after it unmaps without sampling the X pixmap storage the server has freed.
 * Only valid to call while the window is mapped (the live texture is valid). */
static void snapshot_window(WmCompositor *comp, CompositorWindow *cw)
{
    if (!fbo_available || !cw->texture || cw->pw == 0 || cw->ph == 0) {
        return;
    }

    if (cw->snapshot == 0 || cw->snap_w != cw->pw || cw->snap_h != cw->ph) {
        if (cw->snapshot == 0) {
            glGenTextures(1, &cw->snapshot);
        }
        glBindTexture(GL_TEXTURE_2D, cw->snapshot);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cw->pw, cw->ph, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);
        cw->snap_w = cw->pw;
        cw->snap_h = cw->ph;
    }

    pfn_glBindFramebuffer(GL_FRAMEBUFFER, comp->snapshot_fbo);
    pfn_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, cw->snapshot, 0);
    if (pfn_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        pfn_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    /* Identity copy: vertex and texcoord run the same direction so the
     * snapshot stores pixels in the live texture's orientation. */
    glViewport(0, 0, cw->pw, cw->ph);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, cw->pw, 0, cw->ph, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glDisable(GL_BLEND);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBindTexture(GL_TEXTURE_2D, cw->texture);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(0.0f, 0.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(cw->pw, 0.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(cw->pw, cw->ph);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(0.0f, cw->ph);
    glEnd();
    glEnable(GL_BLEND);

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    pfn_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, comp->screen->width_in_pixels,
               comp->screen->height_in_pixels);
}

static void bind_pixmap(WmCompositor *comp, CompositorWindow *cw)
{
    if (cw->pixmap) {
        xcb_free_pixmap(comp->conn, cw->pixmap);
        cw->pixmap = 0;
    }

    cw->pixmap = xcb_generate_id(comp->conn);
    xcb_composite_name_window_pixmap(comp->conn, cw->window, cw->pixmap);

    /* The named pixmap is whatever X11 rendered — content plus the border
     * X11 drew around it.  Use its real size and draw it verbatim. */
    xcb_get_geometry_reply_t *pg = xcb_get_geometry_reply(comp->conn,
        xcb_get_geometry(comp->conn, cw->pixmap), NULL);
    if (pg) {
        cw->pw = pg->width;
        cw->ph = pg->height;
        free(pg);
    } else {
        cw->pw = cw->width;
        cw->ph = cw->height;
    }
    xcb_flush(comp->conn);

    int bound = (egl_image_available &&
                 bind_pixmap_eglimage(comp, cw) == 0);
    if (!bound) {
        bound = (bind_pixmap_readback(comp, cw) == 0);
    }
    if (!bound) {
        fprintf(stderr, "compositor: failed to bind pixmap for "
                        "window 0x%x\n", cw->window);
        return;
    }

    /* Refresh the owned snapshot from the freshly bound live texture so a
     * later unmap can fade out a valid copy. */
    snapshot_window(comp, cw);
}

/* ---------- init / destroy ---------- */

int wm_compositor_init(Wm *wm)
{
    /* Query Composite extension */
    const xcb_query_extension_reply_t *comp_ext =
        xcb_get_extension_data(wm->conn, &xcb_composite_id);
    if (!comp_ext || !comp_ext->present) {
        fprintf(stderr, "compositor: Composite extension not available\n");
        return -1;
    }

    xcb_composite_query_version_reply_t *cv =
        xcb_composite_query_version_reply(wm->conn,
            xcb_composite_query_version(wm->conn, 0, 4), NULL);
    if (!cv || cv->major_version < 0 ||
        (cv->major_version == 0 && cv->minor_version < 2)) {
        fprintf(stderr, "compositor: Composite >= 0.2 required\n");
        free(cv);
        return -1;
    }

    /* Query Damage extension */
    const xcb_query_extension_reply_t *dmg_ext =
        xcb_get_extension_data(wm->conn, &xcb_damage_id);
    if (!dmg_ext || !dmg_ext->present) {
        fprintf(stderr, "compositor: Damage extension not available\n");
        free(cv);
        return -1;
    }
    xcb_damage_query_version(wm->conn, 1, 1);

    WmCompositor *comp = calloc(1, sizeof(*comp));
    comp->conn = wm->conn;
    comp->screen = wm->screen;
    comp->scale = wm->scale_factor;
    comp->damage_event_base = dmg_ext->first_event;
    comp->composite_major = cv->major_version;
    comp->composite_minor = cv->minor_version;
    free(cv);

    /* Redirect all subwindows to off-screen storage */
    xcb_composite_redirect_subwindows(comp->conn, wm->root,
                                       XCB_COMPOSITE_REDIRECT_MANUAL);

    /* Get the overlay window */
    xcb_composite_get_overlay_window_reply_t *ov =
        xcb_composite_get_overlay_window_reply(comp->conn,
            xcb_composite_get_overlay_window(comp->conn, wm->root), NULL);
    if (!ov) {
        fprintf(stderr, "compositor: failed to get overlay window\n");
        free(comp);
        return -1;
    }
    comp->overlay = ov->overlay_win;
    free(ov);

    /* Initialize EGL with XCB platform */
    EGLint platform_attrs[] = {
        EGL_PLATFORM_XCB_SCREEN_EXT, wm->screen_num,
        EGL_NONE
    };

    PFNEGLGETPLATFORMDISPLAYEXTPROC pfn_getPlatformDisplay =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)
            eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (pfn_getPlatformDisplay) {
        comp->egl_display = pfn_getPlatformDisplay(
            EGL_PLATFORM_XCB_EXT, comp->conn, platform_attrs);
    }
    if (comp->egl_display == EGL_NO_DISPLAY) {
        comp->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (comp->egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "compositor: eglGetDisplay failed\n");
        xcb_composite_release_overlay_window(comp->conn, wm->root);
        free(comp);
        return -1;
    }

    EGLint major, minor;
    if (!eglInitialize(comp->egl_display, &major, &minor)) {
        fprintf(stderr, "compositor: eglInitialize failed\n");
        xcb_composite_release_overlay_window(comp->conn, wm->root);
        free(comp);
        return -1;
    }
    fprintf(stderr, "compositor: EGL %d.%d initialized\n", major, minor);

    eglBindAPI(EGL_OPENGL_API);

    EGLint config_attrs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_NONE
    };
    EGLint num_configs;
    if (!eglChooseConfig(comp->egl_display, config_attrs,
                         &comp->egl_config, 1, &num_configs) ||
        num_configs == 0) {
        fprintf(stderr, "compositor: no suitable EGL config\n");
        eglTerminate(comp->egl_display);
        xcb_composite_release_overlay_window(comp->conn, wm->root);
        free(comp);
        return -1;
    }

    comp->egl_context = eglCreateContext(comp->egl_display, comp->egl_config,
                                          EGL_NO_CONTEXT, NULL);
    if (comp->egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "compositor: eglCreateContext failed\n");
        eglTerminate(comp->egl_display);
        xcb_composite_release_overlay_window(comp->conn, wm->root);
        free(comp);
        return -1;
    }

    comp->egl_surface = eglCreateWindowSurface(comp->egl_display,
                                                comp->egl_config,
                                                comp->overlay, NULL);
    if (comp->egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "compositor: eglCreateWindowSurface failed (0x%x)\n",
                eglGetError());
        eglDestroyContext(comp->egl_display, comp->egl_context);
        eglTerminate(comp->egl_display);
        xcb_composite_release_overlay_window(comp->conn, wm->root);
        free(comp);
        return -1;
    }

    eglMakeCurrent(comp->egl_display, comp->egl_surface,
                    comp->egl_surface, comp->egl_context);

    load_extensions();

    eglSwapInterval(comp->egl_display, 0);

    /* Allow input to pass through the overlay.
     * Must happen AFTER eglCreateWindowSurface which may reconfigure
     * the overlay window and reset its shape.
     * Use Shape extension directly (xcb_shape_rectangles with 0 rects)
     * for widest server compatibility. */
    xcb_shape_rectangles(comp->conn,
                          XCB_SHAPE_SO_SET,
                          XCB_SHAPE_SK_INPUT,
                          XCB_CLIP_ORDERING_UNSORTED,
                          comp->overlay,
                          0, 0, 0, NULL);
    xcb_flush(comp->conn);

    /* Set up orthographic projection matching screen size */
    glViewport(0, 0, comp->screen->width_in_pixels,
               comp->screen->height_in_pixels);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, comp->screen->width_in_pixels,
            comp->screen->height_in_pixels, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (fbo_available) {
        pfn_glGenFramebuffers(1, &comp->snapshot_fbo);
    }

    comp->needs_repaint = 1;
    wm->compositor = comp;

    /* Composite any windows already mapped before the compositor started
     * (adopted clients, panel, etc.). New windows arrive via MapNotify. */
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(comp->conn,
        xcb_query_tree(comp->conn, wm->root), NULL);
    if (tree) {
        xcb_window_t *children = xcb_query_tree_children(tree);
        int n = xcb_query_tree_children_length(tree);
        for (int i = 0; i < n; i++) {
            xcb_get_window_attributes_reply_t *attr =
                xcb_get_window_attributes_reply(comp->conn,
                    xcb_get_window_attributes(comp->conn, children[i]), NULL);
            if (attr) {
                if (attr->map_state == XCB_MAP_STATE_VIEWABLE &&
                    attr->_class != XCB_WINDOW_CLASS_INPUT_ONLY) {
                    wm_compositor_add_window(comp, children[i]);
                }
                free(attr);
            }
        }
        free(tree);
    }

    fprintf(stderr, "compositor: initialized (overlay 0x%x)\n", comp->overlay);
    return 0;
}

void wm_compositor_destroy(WmCompositor *comp)
{
    if (!comp) {
        return;
    }

    while (comp->windows) {
        CompositorWindow *cw = comp->windows;
        comp->windows = cw->next;
        if (cw->damage) {
            xcb_damage_destroy(comp->conn, cw->damage);
        }
        if (cw->texture) {
            glDeleteTextures(1, &cw->texture);
        }
        if (cw->snapshot) {
            glDeleteTextures(1, &cw->snapshot);
        }
        if (cw->pixmap) {
            xcb_free_pixmap(comp->conn, cw->pixmap);
        }
        free(cw);
    }

    if (comp->snapshot_fbo) {
        pfn_glDeleteFramebuffers(1, &comp->snapshot_fbo);
    }
    if (comp->switcher_title_tex) {
        glDeleteTextures(1, &comp->switcher_title_tex);
    }
    free(comp->switcher_wins);

    eglMakeCurrent(comp->egl_display, EGL_NO_SURFACE,
                    EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(comp->egl_display, comp->egl_surface);
    eglDestroyContext(comp->egl_display, comp->egl_context);
    eglTerminate(comp->egl_display);

    xcb_composite_release_overlay_window(comp->conn,
                                          comp->screen->root);
    xcb_composite_unredirect_subwindows(comp->conn, comp->screen->root,
                                         XCB_COMPOSITE_REDIRECT_MANUAL);
    xcb_flush(comp->conn);
    free(comp);
}

/* ---------- fade animation ---------- */

#define FADE_DURATION_MS 150
#define SLIDE_DURATION_MS 250

/* Alt+Tab preview row. */
#define SWITCHER_VISIBLE   5      /* odd: the center slot is the selection */
#define SWITCHER_CYCLE_MS  150
#define SWITCHER_SLOT_W    0.16f  /* slot pitch as a fraction of screen width */
#define SWITCHER_THUMB_H   0.20f  /* thumbnail box height as a fraction of screen height */
#define SWITCHER_TITLE_PAD 12     /* gap below the row, before the title (px) */

static uint64_t comp_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------- window management ---------- */

void wm_compositor_add_window(WmCompositor *comp, xcb_window_t win)
{
    if (find_comp_window(comp, win)) {
        return;
    }

    /* Never composite our own overlay window — texturing it onto itself is
     * a feedback loop that redraws the previous frame every paint. */
    if (win == comp->overlay) {
        return;
    }

    /* Input-only windows have no pixmap; NameWindowPixmap would BadMatch */
    xcb_get_window_attributes_reply_t *attr =
        xcb_get_window_attributes_reply(comp->conn,
            xcb_get_window_attributes(comp->conn, win), NULL);
    if (attr) {
        int input_only = (attr->_class == XCB_WINDOW_CLASS_INPUT_ONLY);
        free(attr);
        if (input_only) {
            return;
        }
    }

    xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(comp->conn,
        xcb_get_geometry(comp->conn, win), NULL);
    if (!geo) {
        return;
    }

    CompositorWindow *cw = calloc(1, sizeof(*cw));
    cw->window = win;
    cw->x = geo->x;
    cw->y = geo->y;
    cw->width = geo->width;
    cw->height = geo->height;
    cw->border = geo->border_width;
    cw->mapped = 1;
    cw->dirty = 1;
    cw->opacity = 0.0f;
    cw->fade_dir = 1;
    cw->fade_last_ms = comp_now_ms();
    free(geo);

    cw->damage = xcb_generate_id(comp->conn);
    xcb_damage_create(comp->conn, cw->damage, win,
                       XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);

    bind_pixmap(comp, cw);

    cw->next = comp->windows;
    comp->windows = cw;
    comp->needs_repaint = 1;

    xcb_flush(comp->conn);
}

void wm_compositor_remove_window(WmCompositor *comp, xcb_window_t win)
{
    CompositorWindow **pp = &comp->windows;
    while (*pp && (*pp)->window != win) {
        pp = &(*pp)->next;
    }
    if (!*pp) {
        return;
    }

    CompositorWindow *cw = *pp;
    *pp = cw->next;

    if (cw->damage) {
        xcb_damage_destroy(comp->conn, cw->damage);
    }
    if (cw->texture) {
        glDeleteTextures(1, &cw->texture);
    }
    if (cw->snapshot) {
        glDeleteTextures(1, &cw->snapshot);
    }
    if (cw->pixmap) {
        xcb_free_pixmap(comp->conn, cw->pixmap);
    }
    free(cw);
    comp->needs_repaint = 1;
}

void wm_compositor_set_mapped(WmCompositor *comp, xcb_window_t win, int mapped)
{
    CompositorWindow *cw = find_comp_window(comp, win);
    if (!cw) {
        return;
    }
    cw->mapped = mapped;
    if (comp->slide_active && win != comp->slide_exclude) {
        /* During a desktop switch the window slides rather than fades: tag it
         * incoming/outgoing, keep it fully opaque, and suppress the fade.
         * An unmapped window keeps its snapshot so it can slide out. */
        cw->slide_role = mapped ? 1 : -1;
        cw->fade_dir = 0;
        cw->opacity = 1.0f;
    } else {
        cw->fade_dir = mapped ? 1 : -1;
    }
    cw->fade_last_ms = comp_now_ms();
    if (mapped) {
        /* Re-fetch the storage pixmap on next paint — the old one is
         * stale after an unmap/remap cycle. */
        cw->dirty = 1;
        /* The server raises a window to the top when it is mapped, but a plain
         * remap (e.g. a desktop switch) carries no restack ConfigureNotify, so
         * an already-tracked window would keep its stale paint position and
         * render behind others.  Re-sync the paint order to the server tree. */
        compositor_restack_from_tree(comp);
    }
    /* On unmap we keep the bound texture so the last frame can fade out;
     * the window stays in the paint list until its opacity reaches 0. */
    comp->needs_repaint = 1;
}

void wm_compositor_slide(WmCompositor *comp, int dx, int dy)
{
    /* Finalize any slide still in flight: windows that never got remapped were
     * outgoing and should be gone, and stale roles must not bleed into the new
     * slide. */
    for (CompositorWindow *cw = comp->windows; cw; cw = cw->next) {
        if (!cw->mapped) {
            cw->opacity = 0.0f;
        }
        cw->slide_role = 0;
    }

    comp->slide_active = 1;
    comp->slide_dx = dx;
    comp->slide_dy = dy;
    comp->slide_progress = 0.0f;
    comp->slide_last_ms = comp_now_ms();
    comp->slide_start_ms = comp->slide_last_ms;
    comp->needs_repaint = 1;
}

void wm_compositor_set_slide_exclude(WmCompositor *comp, xcb_window_t win)
{
    comp->slide_exclude = win;
}

/* ---------- Alt+Tab preview switcher ---------- */

/* Render the centered window's title to a cairo image surface and upload it as
 * an opaque GL texture.  Opaque background avoids premultiplied-alpha edge
 * artifacts and gives the label a readable plate. */
static void make_switcher_title_texture(WmCompositor *comp, const char *text)
{
    if (comp->switcher_title_tex) {
        glDeleteTextures(1, &comp->switcher_title_tex);
        comp->switcher_title_tex = 0;
    }
    if (!text || !*text) {
        return;
    }

    const IsdeColorScheme *scheme = isde_theme_current();
    int font_px = (int)(isde_font_height("general", 4) * comp->scale + 0.5);
    if (font_px < 10) {
        font_px = 10;
    }
    int h = font_px + (int)(12 * comp->scale + 0.5);
    int w = (int)(comp->screen->width_in_pixels * (SWITCHER_SLOT_W * SWITCHER_VISIBLE));
    if (w < 1) {
        w = 1;
    }

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return;
    }
    cairo_t *cr = cairo_create(surf);
    unsigned int bg = scheme ? scheme->bg : 0x333333;
    unsigned int fg = scheme ? scheme->fg : 0xFFFFFF;
    render_fill_rect(cr, bg, 0, 0, w, h);
    render_text_centered(cr, text, fg, 0, 0, w, h, font_px);
    cairo_destroy(cr);
    cairo_surface_flush(surf);

    unsigned char *data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);

    glGenTextures(1, &comp->switcher_title_tex);
    glBindTexture(GL_TEXTURE_2D, comp->switcher_title_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    comp->switcher_title_w = w;
    comp->switcher_title_h = h;
    cairo_surface_destroy(surf);
}

static void draw_solid_quad(float x, float y, float w, float h,
                            float r, float g, float b, float a)
{
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
    glEnable(GL_TEXTURE_2D);
}

static void draw_solid_round_rect(float x, float y, float w, float h, float r,
                                  float cr, float cg, float cb, float ca)
{
    if (r > w / 2.0f) {
        r = w / 2.0f;
    }
    if (r > h / 2.0f) {
        r = h / 2.0f;
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
    glColor4f(cr, cg, cb, ca);

    /* Two overlapping bands cover everything but the four corners. */
    glBegin(GL_QUADS);
    glVertex2f(x + r, y);
    glVertex2f(x + w - r, y);
    glVertex2f(x + w - r, y + h);
    glVertex2f(x + r, y + h);

    glVertex2f(x, y + r);
    glVertex2f(x + w, y + r);
    glVertex2f(x + w, y + h - r);
    glVertex2f(x, y + h - r);
    glEnd();

    /* Quarter-circle fans fill the corners. */
    const int seg = 6;
    float corners[4][3] = {
        { x + r,     y + r,     180.0f },
        { x + w - r, y + r,     270.0f },
        { x + w - r, y + h - r,   0.0f },
        { x + r,     y + h - r,  90.0f },
    };
    for (int c = 0; c < 4; c++) {
        glBegin(GL_TRIANGLE_FAN);
        glVertex2f(corners[c][0], corners[c][1]);
        for (int s = 0; s <= seg; s++) {
            float a = (corners[c][2] + 90.0f * s / seg) *
                      3.14159265358979f / 180.0f;
            glVertex2f(corners[c][0] + r * cosf(a),
                       corners[c][1] + r * sinf(a));
        }
        glEnd();
    }

    glEnable(GL_TEXTURE_2D);
}

static void draw_textured_quad(GLuint tex, float x, float y, float w, float h)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(x, y);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(x + w, y);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(x + w, y + h);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(x, y + h);
    glEnd();
}

static void draw_switcher(WmCompositor *comp)
{
    if (comp->switcher_count <= 0) {
        return;
    }

    int W = comp->screen->width_in_pixels;
    int H = comp->screen->height_in_pixels;
    const IsdeColorScheme *scheme = isde_theme_current();

    float pitch = W * SWITCHER_SLOT_W;
    float box_h = H * SWITCHER_THUMB_H;
    float box_w = pitch * 0.88f;
    float row_cy = H * 0.46f;
    float center_x = W * 0.5f;
    float xoff = comp->switcher_anim_dir * pitch * comp->switcher_anim;
    float frame_m = 4.0f;
    float panel_pad = 16.0f * comp->scale;

    int clip_w = (int)(pitch * SWITCHER_VISIBLE);
    int clip_x = (int)(center_x - clip_w / 2.0f);

    /* Dim the whole screen, then draw one panel behind the entire switcher
     * (preview row plus the title beneath it). */
    draw_solid_quad(0, 0, W, H, 0.0f, 0.0f, 0.0f, 0.55f);

    float title_h = comp->switcher_title_tex ? (float)comp->switcher_title_h : 0.0f;
    float panel_top = row_cy - box_h / 2.0f - frame_m - panel_pad;
    float panel_bottom = row_cy + box_h / 2.0f + SWITCHER_TITLE_PAD +
                         title_h + panel_pad;
    float radius = 3.0f * comp->scale;
    unsigned int panel = scheme ? scheme->bg : 0x333333;
    draw_solid_round_rect(center_x - clip_w / 2.0f - panel_pad, panel_top,
                          clip_w + 2 * panel_pad, panel_bottom - panel_top,
                          radius,
                          ((panel >> 16) & 0xFF) / 255.0f,
                          ((panel >> 8) & 0xFF) / 255.0f,
                          (panel & 0xFF) / 255.0f, 1.0f);

    /* Clip horizontally to exactly SWITCHER_VISIBLE slots so the extra slots
     * drawn for a smooth slide stay hidden at rest.  Scissor is in framebuffer
     * coords (origin bottom-left). */
    glEnable(GL_SCISSOR_TEST);
    glScissor(clip_x, 0, clip_w, H);

    int half = SWITCHER_VISIBLE / 2;
    for (int o = -half - 1; o <= half + 1; o++) {
        /* Always wrap so a short list cycles through its windows to fill the
         * row, rather than leaving edge slots empty. */
        int idx = comp->switcher_sel + o;
        idx = ((idx % comp->switcher_count) + comp->switcher_count) %
              comp->switcher_count;

        float slot_cx = center_x + o * pitch + xoff;

        /* Stationary selection frame at the center slot, drawn slightly larger
         * than the thumbnail so a themed border shows around it. */
        if (o == 0 && scheme) {
            float mx = 12.0f * comp->scale;  /* wider than the thumbnail */
            float my = -6.0f * comp->scale;  /* shorter than the thumbnail */
            float hw = box_w + 2 * mx;
            float hh = box_h + 2 * my;
            unsigned int a = scheme->active;
            draw_solid_round_rect(center_x - hw / 2, row_cy - hh / 2, hw, hh,
                                  radius,
                                  ((a >> 16) & 0xFF) / 255.0f,
                                  ((a >> 8) & 0xFF) / 255.0f,
                                  (a & 0xFF) / 255.0f, 1.0f);
        }

        CompositorWindow *cw = find_comp_window(comp, comp->switcher_wins[idx]);
        if (cw && cw->snapshot && cw->snap_w && cw->snap_h) {
            float s = box_w / cw->snap_w;
            float sy = box_h / cw->snap_h;
            if (sy < s) {
                s = sy;
            }
            float dw = cw->snap_w * s;
            float dh = cw->snap_h * s;
            draw_textured_quad(cw->snapshot, slot_cx - dw / 2,
                               row_cy - dh / 2, dw, dh);
        } else {
            draw_solid_quad(slot_cx - box_w / 2, row_cy - box_h / 2,
                            box_w, box_h, 0.25f, 0.25f, 0.25f, 1.0f);
        }
    }

    glDisable(GL_SCISSOR_TEST);

    if (comp->switcher_title_tex) {
        float tw = comp->switcher_title_w;
        float th = comp->switcher_title_h;
        draw_textured_quad(comp->switcher_title_tex, center_x - tw / 2,
                           row_cy + box_h / 2 + SWITCHER_TITLE_PAD, tw, th);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
}

void wm_compositor_switcher_begin(WmCompositor *comp, const xcb_window_t *wins,
                                  int count, int sel, const char *sel_title)
{
    free(comp->switcher_wins);
    comp->switcher_wins = NULL;
    comp->switcher_count = 0;

    if (count > 0 && wins) {
        comp->switcher_wins = malloc(count * sizeof(*comp->switcher_wins));
        if (comp->switcher_wins) {
            memcpy(comp->switcher_wins, wins, count * sizeof(*wins));
            comp->switcher_count = count;
        }
    }

    comp->switcher_active = 1;
    comp->switcher_sel = sel;
    comp->switcher_anim = 0.0f;
    comp->switcher_anim_dir = 0;
    make_switcher_title_texture(comp, sel_title);
    comp->needs_repaint = 1;
}

void wm_compositor_switcher_update(WmCompositor *comp, int sel, int dir,
                                   const char *sel_title)
{
    if (!comp->switcher_active) {
        return;
    }
    comp->switcher_sel = sel;
    comp->switcher_anim = 1.0f;
    comp->switcher_anim_dir = dir;
    comp->switcher_anim_last_ms = comp_now_ms();
    make_switcher_title_texture(comp, sel_title);
    comp->needs_repaint = 1;
}

void wm_compositor_switcher_end(WmCompositor *comp)
{
    comp->switcher_active = 0;
    free(comp->switcher_wins);
    comp->switcher_wins = NULL;
    comp->switcher_count = 0;
    comp->switcher_anim = 0.0f;
    if (comp->switcher_title_tex) {
        glDeleteTextures(1, &comp->switcher_title_tex);
        comp->switcher_title_tex = 0;
    }
    comp->needs_repaint = 1;
}

void wm_compositor_window_configured(WmCompositor *comp, xcb_window_t win,
                                      int16_t x, int16_t y,
                                      uint16_t width, uint16_t height,
                                      uint16_t border,
                                      xcb_window_t above_sibling)
{
    CompositorWindow *cw = find_comp_window(comp, win);
    if (!cw) {
        return;
    }

    /* The named pixmap covers the window plus its border, so a border
     * change resizes the pixmap and needs a rebind too. */
    int resized = (cw->width != width || cw->height != height ||
                   cw->border != border);
    cw->x = x;
    cw->y = y;
    cw->width = width;
    cw->height = height;
    cw->border = border;

    if (resized && cw->mapped) {
        bind_pixmap(comp, cw);
    } else if (resized) {
        /* Unmapped: defer the rebind to the next map (set_mapped sets dirty). */
        cw->dirty = 1;
    }

    /* A change in above_sibling means the window was restacked.  Rebuild the
     * paint order from the server's actual stacking; skip when it is unchanged
     * so move/resize ConfigureNotify floods (e.g. interactive drags) don't
     * each trigger a QueryTree round-trip. */
    if (cw->above != above_sibling) {
        cw->above = above_sibling;
        compositor_restack_from_tree(comp);
    }

    comp->needs_repaint = 1;
}

/* ---------- damage ---------- */

void wm_compositor_handle_damage(WmCompositor *comp,
                                  xcb_damage_notify_event_t *ev)
{
    CompositorWindow *cw = find_comp_window(comp, ev->drawable);
    if (!cw) {
        return;
    }

    xcb_damage_subtract(comp->conn, cw->damage, XCB_NONE, XCB_NONE);

    cw->dirty = 1;
    comp->needs_repaint = 1;
}

/* ---------- paint ---------- */

void wm_compositor_paint(WmCompositor *comp)
{
    if (!comp->needs_repaint) {
        return;
    }
    comp->needs_repaint = 0;

    /* Rebind pixmaps for dirty, mapped windows.  NameWindowPixmap fails on
     * unmapped windows, so skip them — they aren't painted anyway. */
    for (CompositorWindow *cw = comp->windows; cw; cw = cw->next) {
        if (cw->dirty && cw->mapped) {
            bind_pixmap(comp, cw);
            cw->dirty = 0;
        }
    }

    /* Advance fade animations by elapsed wall-clock time so the speed is
     * independent of frame rate.  Re-arm needs_repaint while any window is
     * still animating so the next loop iteration paints the next frame. */
    {
        uint64_t now = comp_now_ms();
        for (CompositorWindow *cw = comp->windows; cw; cw = cw->next) {
            if (cw->fade_dir == 0) {
                continue;
            }
            uint64_t dt = now - cw->fade_last_ms;
            cw->fade_last_ms = now;
            cw->opacity += cw->fade_dir * (float)dt / FADE_DURATION_MS;
            /* Only complete in the direction we're actually fading.  A dt==0
             * first frame leaves opacity at the start value (1.0 fading out,
             * 0.0 fading in); a direction-blind check would mistake that for
             * completion and freeze the fade. */
            if (cw->fade_dir > 0 && cw->opacity >= 1.0f) {
                cw->opacity = 1.0f;
                cw->fade_dir = 0;
            } else if (cw->fade_dir < 0 && cw->opacity <= 0.0f) {
                cw->opacity = 0.0f;
                cw->fade_dir = 0;
            } else {
                comp->needs_repaint = 1;
            }
        }
    }

    /* Advance the desktop-switch slide.  Hold at progress 0 until the map/unmap
     * events have tagged the participating windows, so the slide doesn't begin
     * before there is anything to move.  A switch between empty desktops never
     * tags anything, so cap the wait to avoid spinning forever. */
    if (comp->slide_active) {
        int tagged = 0;
        for (CompositorWindow *cw = comp->windows; cw; cw = cw->next) {
            if (cw->slide_role != 0) {
                tagged = 1;
                break;
            }
        }

        uint64_t now = comp_now_ms();
        uint64_t dt = now - comp->slide_last_ms;
        comp->slide_last_ms = now;

        if (tagged) {
            comp->slide_progress += (float)dt / SLIDE_DURATION_MS;
        }

        int done = (tagged && comp->slide_progress >= 1.0f) ||
                   (!tagged && now - comp->slide_start_ms > SLIDE_DURATION_MS);
        if (done) {
            comp->slide_progress = 1.0f;
            comp->slide_active = 0;
            for (CompositorWindow *cw = comp->windows; cw; cw = cw->next) {
                if (cw->slide_role < 0) {
                    cw->opacity = 0.0f;  /* outgoing window is now off-screen */
                }
                cw->slide_role = 0;
            }
        } else {
            comp->needs_repaint = 1;
        }
    }

    /* Advance the switcher cycle animation (row slides one slot on Tab). */
    if (comp->switcher_anim > 0.0f) {
        uint64_t now = comp_now_ms();
        uint64_t dt = now - comp->switcher_anim_last_ms;
        comp->switcher_anim_last_ms = now;
        comp->switcher_anim -= (float)dt / SWITCHER_CYCLE_MS;
        if (comp->switcher_anim <= 0.0f) {
            comp->switcher_anim = 0.0f;
        } else {
            comp->needs_repaint = 1;
        }
    }

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* Paint windows in list order (last = frontmost, drawn last).
     * The list is prepend-order so we need to walk it in reverse.
     * Build a temporary array for reverse iteration. */
    int count = 0;
    for (CompositorWindow *cw = comp->windows; cw; cw = cw->next) {
        count++;
    }

    if (count > 0) {
        CompositorWindow **order = malloc(count * sizeof(*order));
        int i = count - 1;
        for (CompositorWindow *cw = comp->windows; cw; cw = cw->next) {
            order[i--] = cw;
        }

        for (i = 0; i < count; i++) {
            CompositorWindow *cw = order[i];
            /* Draw the owned snapshot so fade-out doesn't sample freed pixmap
             * storage; fall back to the live texture only while mapped (e.g.
             * when no FBO is available to snapshot). */
            GLuint tex = cw->snapshot ? cw->snapshot
                                      : (cw->mapped ? cw->texture : 0);
            /* Paint mapped windows, and unmapped ones still fading out.
             * Skip when fully transparent and unmapped, or with no texture. */
            if ((!cw->mapped && cw->opacity <= 0.0f) || !tex) {
                continue;
            }

            glBindTexture(GL_TEXTURE_2D, tex);
            glColor4f(1.0f, 1.0f, 1.0f, cw->opacity);

            /* Desktop-switch slide offset.  Outgoing windows slide off toward
             * the travel edge; incoming windows slide in from the opposite
             * edge.  Windows not in the slide (e.g. sticky panels, the switch
             * OSD) stay put. */
            float ox = 0.0f, oy = 0.0f;
            int slide_part = (cw->slide_role != 0 &&
                              cw->window != comp->slide_exclude);
            if (slide_part && cw->slide_role > 0) {
                ox = (1.0f - comp->slide_progress) * comp->slide_dx *
                     comp->screen->width_in_pixels;
                oy = (1.0f - comp->slide_progress) * comp->slide_dy *
                     comp->screen->height_in_pixels;
            } else if (slide_part && cw->slide_role < 0) {
                ox = -comp->slide_progress * comp->slide_dx *
                     comp->screen->width_in_pixels;
                oy = -comp->slide_progress * comp->slide_dy *
                     comp->screen->height_in_pixels;
            }

            /* Draw the named pixmap verbatim at the window's outer corner —
             * it already contains the border X11 drew. */
            float x0 = cw->x + ox;
            float y0 = cw->y + oy;
            float x1 = cw->x + cw->pw + ox;
            float y1 = cw->y + cw->ph + oy;

            glBegin(GL_QUADS);
            glTexCoord2f(0.0f, 0.0f); glVertex2f(x0, y0);
            glTexCoord2f(1.0f, 0.0f); glVertex2f(x1, y0);
            glTexCoord2f(1.0f, 1.0f); glVertex2f(x1, y1);
            glTexCoord2f(0.0f, 1.0f); glVertex2f(x0, y1);
            glEnd();
        }

        free(order);
    }

    if (comp->switcher_active) {
        draw_switcher(comp);
    }

    eglSwapBuffers(comp->egl_display, comp->egl_surface);
}

int wm_compositor_animating(WmCompositor *comp)
{
    if (comp->slide_active || comp->switcher_anim > 0.0f) {
        return 1;
    }
    for (CompositorWindow *cw = comp->windows; cw; cw = cw->next) {
        if (cw->fade_dir != 0) {
            return 1;
        }
    }
    return 0;
}
