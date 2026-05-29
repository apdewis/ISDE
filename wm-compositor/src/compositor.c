#define _POSIX_C_SOURCE 200809L

#include "compositor.h"
#include "wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/shape.h>
#include <xcb/xcb_aux.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

/* EGL / GL extension function pointers (zero-copy path) */
static PFNEGLCREATEIMAGEKHRPROC   pfn_eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC  pfn_eglDestroyImageKHR;

typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum target, void *image);
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pfn_glEGLImageTargetTexture2DOES;

static int egl_image_available;

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
    if (!cw->pixmap || cw->width == 0 || cw->height == 0) {
        return -1;
    }

    xcb_get_image_reply_t *img = xcb_get_image_reply(comp->conn,
        xcb_get_image(comp->conn, XCB_IMAGE_FORMAT_Z_PIXMAP,
                      cw->pixmap, 0, 0, cw->width, cw->height,
                      0xFFFFFFFF), NULL);
    if (!img) {
        return -1;
    }

    uint8_t *data = xcb_get_image_data(img);
    int len = xcb_get_image_data_length(img);
    int npixels = cw->width * cw->height;
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
                 cw->width, cw->height, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);

    free(img);
    return 0;
}

static void bind_pixmap(WmCompositor *comp, CompositorWindow *cw)
{
    if (cw->pixmap) {
        xcb_free_pixmap(comp->conn, cw->pixmap);
        cw->pixmap = 0;
    }

    cw->pixmap = xcb_generate_id(comp->conn);
    xcb_composite_name_window_pixmap(comp->conn, cw->window, cw->pixmap);
    xcb_flush(comp->conn);

    if (egl_image_available &&
        bind_pixmap_eglimage(comp, cw) == 0) {
        return;
    }

    if (bind_pixmap_readback(comp, cw) != 0) {
        fprintf(stderr, "compositor: failed to bind pixmap for "
                        "window 0x%x\n", cw->window);
    }
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
        if (cw->pixmap) {
            xcb_free_pixmap(comp->conn, cw->pixmap);
        }
        free(cw);
    }

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
    cw->mapped = 1;
    cw->dirty = 1;
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
    if (mapped) {
        /* Re-fetch the storage pixmap on next paint — the old one is
         * stale after an unmap/remap cycle. */
        cw->dirty = 1;
    }
    comp->needs_repaint = 1;
}

void wm_compositor_window_configured(WmCompositor *comp, xcb_window_t win,
                                      int16_t x, int16_t y,
                                      uint16_t width, uint16_t height)
{
    CompositorWindow *cw = find_comp_window(comp, win);
    if (!cw) {
        return;
    }

    int resized = (cw->width != width || cw->height != height);
    cw->x = x;
    cw->y = y;
    cw->width = width;
    cw->height = height;

    if (resized) {
        bind_pixmap(comp, cw);
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
            if (!cw->mapped || !cw->texture) {
                continue;
            }

            glBindTexture(GL_TEXTURE_2D, cw->texture);
            glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

            float x0 = cw->x;
            float y0 = cw->y;
            float x1 = cw->x + cw->width;
            float y1 = cw->y + cw->height;

            glBegin(GL_QUADS);
            glTexCoord2f(0.0f, 0.0f); glVertex2f(x0, y0);
            glTexCoord2f(1.0f, 0.0f); glVertex2f(x1, y0);
            glTexCoord2f(1.0f, 1.0f); glVertex2f(x1, y1);
            glTexCoord2f(0.0f, 1.0f); glVertex2f(x0, y1);
            glEnd();
        }

        free(order);
    }

    eglSwapBuffers(comp->egl_display, comp->egl_surface);
}
