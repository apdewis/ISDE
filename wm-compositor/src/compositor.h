#ifndef ISDE_COMPOSITOR_H
#define ISDE_COMPOSITOR_H

#include <xcb/xcb.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <EGL/egl.h>
#include <GL/gl.h>

typedef struct CompositorWindow {
    xcb_window_t        window;
    xcb_pixmap_t        pixmap;
    xcb_damage_damage_t damage;
    GLuint              texture;         /* live pixmap texture (may alias X storage) */
    GLuint              snapshot;        /* owned copy of last mapped frame, for fade-out */
    uint16_t            snap_w, snap_h;  /* snapshot texture size */
    int16_t             x, y;
    uint16_t            width, height;   /* window content size */
    uint16_t            border;          /* X border width */
    uint16_t            pw, ph;          /* pixmap size (incl. border, drawn as-is) */
    int                 dirty;
    int                 mapped;
    float               opacity;         /* current rendered opacity, 0..1 */
    int                 fade_dir;        /* +1 fading in, -1 fading out, 0 idle */
    uint64_t            fade_last_ms;    /* timestamp of last opacity advance */
    xcb_window_t        above;           /* last-seen above_sibling, for restack detection */
    struct CompositorWindow *next;
} CompositorWindow;

typedef struct WmCompositor {
    xcb_connection_t   *conn;
    xcb_screen_t       *screen;
    xcb_window_t        overlay;

    uint8_t             damage_event_base;
    uint8_t             composite_major;
    uint8_t             composite_minor;

    EGLDisplay          egl_display;
    EGLContext          egl_context;
    EGLSurface          egl_surface;
    EGLConfig           egl_config;

    GLuint              quad_program;
    GLuint              snapshot_fbo;    /* FBO for copying live textures into snapshots */

    CompositorWindow   *windows;
    int                 needs_repaint;
} WmCompositor;

struct Wm;

int   wm_compositor_init(struct Wm *wm);
void  wm_compositor_destroy(WmCompositor *comp);
void  wm_compositor_add_window(WmCompositor *comp, xcb_window_t win);
void  wm_compositor_remove_window(WmCompositor *comp, xcb_window_t win);
void  wm_compositor_set_mapped(WmCompositor *comp, xcb_window_t win, int mapped);
void  wm_compositor_paint(WmCompositor *comp);
int   wm_compositor_animating(WmCompositor *comp);
void  wm_compositor_handle_damage(WmCompositor *comp,
                                   xcb_damage_notify_event_t *ev);
void  wm_compositor_window_configured(WmCompositor *comp, xcb_window_t win,
                                       int16_t x, int16_t y,
                                       uint16_t width, uint16_t height,
                                       uint16_t border,
                                       xcb_window_t above_sibling);

#endif /* ISDE_COMPOSITOR_H */
