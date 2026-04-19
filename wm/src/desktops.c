#define _POSIX_C_SOURCE 200809L
/*
 * desktops.c — virtual desktop management
 *
 * Configurable grid of desktops (rows x cols). Ctrl+Super+Arrow
 * navigates between them. An OSD popup shows the grid with the
 * active desktop highlighted.
 */
#include "wm.h"
#include "isde/isde-config.h"
#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- config ---------- */

void wm_desktops_init(Wm *wm)
{
    /* Defaults */
    wm->desk_rows = 1;
    wm->desk_cols = 2;

    /* Read from config */
    char errbuf[256];
    IsdeConfig *cfg = isde_config_load_xdg("isde.toml", errbuf, sizeof(errbuf));
    if (cfg) {
        IsdeConfigTable *root = isde_config_root(cfg);
        IsdeConfigTable *wm_tbl = isde_config_table(root, "wm");
        IsdeConfigTable *desk = wm_tbl ? isde_config_table(wm_tbl, "desktops") : NULL;
        if (desk) {
            int r = (int)isde_config_int(desk, "rows", 0);
            int c = (int)isde_config_int(desk, "columns", 0);
            if (r > 0) { wm->desk_rows = r; }
            if (c > 0) { wm->desk_cols = c; }
        }
        isde_config_free(cfg);
    }

    wm->num_desktops = wm->desk_rows * wm->desk_cols;
    wm->current_desktop = 0;

    /* Set EWMH properties */
    isde_ewmh_set_number_of_desktops(wm->ewmh, wm->num_desktops);
    isde_ewmh_set_current_desktop(wm->ewmh, 0);
    xcb_flush(wm->conn);

    fprintf(stderr, "isde-wm: desktops: %dx%d grid (%d total)\n",
            wm->desk_cols, wm->desk_rows, wm->num_desktops);
}

/* ---------- switching ---------- */

void wm_desktops_switch(Wm *wm, uint32_t desktop)
{
    if (desktop >= (uint32_t)wm->num_desktops) {
        return;
    }
    if (desktop == wm->current_desktop) {
        return;
    }

    uint32_t old = wm->current_desktop;
    wm->current_desktop = desktop;

    /* Map/unmap windows based on desktop assignment */
    for (WmClient *c = wm->clients; c; c = c->next) {
        /* Sticky windows (0xFFFFFFFF) are always visible */
        if (c->desktop == 0xFFFFFFFF) {
            continue;
        }

        if (c->desktop == old && c->desktop != desktop) {
            /* Hide: unmap the frame */
            if (c->shell && IswIsRealized(c->shell)) {
                xcb_unmap_window(wm->conn, IswWindow(c->shell));
            }
        } else if (c->desktop == desktop && c->desktop != old) {
            /* Show: map the frame */
            if (c->shell && IswIsRealized(c->shell)) {
                xcb_map_window(wm->conn, IswWindow(c->shell));
            }
        }
    }

    /* Update EWMH */
    isde_ewmh_set_current_desktop(wm->ewmh, desktop);

    /* If the focused window is on the old desktop, clear focus */
    if (wm->focused && wm->focused->desktop != desktop &&
        wm->focused->desktop != 0xFFFFFFFF) {
        wm->focused->focused = 0;
        frame_apply_theme(wm, wm->focused);
        wm->focused = NULL;
        wm_ewmh_update_active(wm);
    }

    xcb_flush(wm->conn);

    wm_desktops_show_osd(wm);
}

/* Move relative to current position in the grid */
void wm_desktops_move(Wm *wm, int dx, int dy)
{
    int col = wm->current_desktop % wm->desk_cols;
    int row = wm->current_desktop / wm->desk_cols;

    col += dx;
    row += dy;

    /* Wrap */
    if (col < 0) { col = wm->desk_cols - 1; }
    if (col >= wm->desk_cols) { col = 0; }
    if (row < 0) { row = wm->desk_rows - 1; }
    if (row >= wm->desk_rows) { row = 0; }

    uint32_t target = row * wm->desk_cols + col;
    wm_desktops_switch(wm, target);
}

/* ---------- OSD popup ---------- */

#define OSD_CELL   30
#define OSD_GAP    4
#define OSD_PAD    8
#define OSD_TIMEOUT 800  /* ms */

static void osd_hide_cb(IswPointer client_data, IswIntervalId *id)
{
    (void)id;
    Wm *wm = (Wm *)client_data;
    if (wm->desk_osd && IswIsRealized(wm->desk_osd)) {
        IswPopdown(wm->desk_osd);
    }
    wm->desk_osd_timer = 0;
}

/* Draw the grid using the shell background + colored child windows */
void wm_desktops_show_osd(Wm *wm)
{
    int cols = wm->desk_cols;
    int rows = wm->desk_rows;
    int w = 2 * OSD_PAD + cols * OSD_CELL + (cols - 1) * OSD_GAP;
    int h = 2 * OSD_PAD + rows * OSD_CELL + (rows - 1) * OSD_GAP;

    /* Center on screen — all values logical since ISW scales shell
     * geometry during creation. */
    double sf = ISWScaleFactor(wm->toplevel);
    int log_sw = (int)(wm->screen->width_in_pixels / sf + 0.5);
    int log_sh = (int)(wm->screen->height_in_pixels / sf + 0.5);
    int sx = (log_sw - w) / 2;
    int sy = (log_sh - h) / 2;

    /* Destroy and recreate each time for simplicity */
    if (wm->desk_osd) {
        if (wm->desk_osd_timer) {
            IswRemoveTimeOut(wm->desk_osd_timer);
            wm->desk_osd_timer = 0;
        }
        IswDestroyWidget(wm->desk_osd);
        wm->desk_osd = NULL;
    }

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgX(&ab, sx);
    IswArgY(&ab, sy);
    IswArgWidth(&ab, w);
    IswArgHeight(&ab, h);
    IswArgOverrideRedirect(&ab, True);
    IswArgBorderWidth(&ab, 1);
    wm->desk_osd = IswCreatePopupShell("desktopOSD",
                                       overrideShellWidgetClass,
                                       wm->toplevel, ab.args, ab.count);

    /* Create a label for each desktop cell */
    const IsdeColorScheme *scheme = isde_theme_current();
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            uint32_t idx = r * cols + c;
            int is_active = (idx == wm->current_desktop);

            char name[16];
            snprintf(name, sizeof(name), "%d", idx + 1);

            IswArgBuilderReset(&ab);
            IswArgLabel(&ab, name);
            IswArgWidth(&ab, OSD_CELL);
            IswArgHeight(&ab, OSD_CELL);
            IswArgBorderWidth(&ab, 1);

            if (scheme && is_active) {
                IswArgBackground(&ab, (Pixel)scheme->active);
                IswArgForeground(&ab, (Pixel)scheme->fg_light);
            }

            Widget cell = IswCreateManagedWidget("deskCell",
                labelWidgetClass, wm->desk_osd, ab.args, ab.count);

            /* Position manually after realize */
            (void)cell;
        }
    }

    IswRealizeWidget(wm->desk_osd);

    /* Position each cell */
    int child_idx = 0;
    CompositeWidget cw = (CompositeWidget)wm->desk_osd;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            if (child_idx < (int)cw->composite.num_children) {
                int cx = OSD_PAD + c * (OSD_CELL + OSD_GAP);
                int cy = OSD_PAD + r * (OSD_CELL + OSD_GAP);
                IswConfigureWidget(cw->composite.children[child_idx],
                                  cx, cy, OSD_CELL, OSD_CELL, 1);
            }
            child_idx++;
        }
    }

    IswPopup(wm->desk_osd, IswGrabNone);

    /* Auto-hide after timeout */
    wm->desk_osd_timer = IswAppAddTimeOut(wm->app, OSD_TIMEOUT,
                                          osd_hide_cb, wm);
}
