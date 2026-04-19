#define _POSIX_C_SOURCE 200809L
/*
 * switcher.c — Alt+Tab window switcher OSD
 *
 * Shows window titles in MRU order.  The selected entry is always
 * vertically centered, with the list wrapping around above and below.
 * The OSD is created once when the switcher activates; subsequent
 * Tab presses just update label text and colors in place.
 * Height is capped at 1/3 of the screen.
 */
#include "wm.h"

#include <ISW/Label.h>
#include <ISW/ISWRender.h>
#include <ISW/IswArgMacros.h>

#include <stdlib.h>
#include <string.h>

#define SWITCHER_WIDTH   300   /* logical pixels */
#define SWITCHER_PAD       8

/* Sort comparator: descending focus_seq (MRU first) */
static int cmp_mru(const void *a, const void *b)
{
    const WmClient *ca = *(const WmClient *const *)a;
    const WmClient *cb = *(const WmClient *const *)b;
    if (ca->focus_seq > cb->focus_seq) return -1;
    if (ca->focus_seq < cb->focus_seq) return  1;
    return 0;
}

/* Build the MRU-ordered client array, filtering to current desktop */
static void build_order(Wm *wm)
{
    int count = 0;
    for (WmClient *c = wm->clients; c; c = c->next) {
        if (c->transient_for) continue;
        if (c->desktop != wm->current_desktop &&
            c->desktop != 0xFFFFFFFF) continue;
        count++;
    }

    free(wm->switcher_order);
    free(wm->switcher_labels);
    wm->switcher_order = NULL;
    wm->switcher_labels = NULL;
    wm->switcher_count = 0;

    if (count == 0) return;

    wm->switcher_order  = malloc(count * sizeof(WmClient *));
    wm->switcher_labels = malloc(count * sizeof(String));
    if (!wm->switcher_order || !wm->switcher_labels) return;

    int i = 0;
    for (WmClient *c = wm->clients; c; c = c->next) {
        if (c->transient_for) continue;
        if (c->desktop != wm->current_desktop &&
            c->desktop != 0xFFFFFFFF) continue;
        wm->switcher_order[i++] = c;
    }

    qsort(wm->switcher_order, count, sizeof(WmClient *), cmp_mru);

    for (i = 0; i < count; i++) {
        wm->switcher_labels[i] = wm->switcher_order[i]->title
                                  ? wm->switcher_order[i]->title
                                  : (String)"(untitled)";
    }

    wm->switcher_count = count;
}

static void destroy_osd(Wm *wm)
{
    if (wm->switcher_shell) {
        IswDestroyWidget(wm->switcher_shell);
        wm->switcher_shell = NULL;
    }
    free(wm->switcher_order);
    wm->switcher_order = NULL;
    free(wm->switcher_labels);
    wm->switcher_labels = NULL;
    wm->switcher_count = 0;
    wm->switcher_visible = 0;
}

/* Map a row index (0..visible-1) to the MRU list index, wrapping.
 * Row center_row corresponds to switcher_sel. */
static int row_to_index(Wm *wm, int row, int center_row)
{
    int offset = row - center_row;
    int idx = (wm->switcher_sel + offset) % wm->switcher_count;
    if (idx < 0) idx += wm->switcher_count;
    return idx;
}

/* Update all label widgets to reflect the current selection */
static void update_labels(Wm *wm)
{
    const IsdeColorScheme *scheme = isde_theme_current();
    CompositeWidget cw = (CompositeWidget)wm->switcher_shell;
    int visible = wm->switcher_visible;
    int center_row = visible / 2;

    for (int i = 0; i < visible && i < (int)cw->composite.num_children; i++) {
        int idx = row_to_index(wm, i, center_row);
        int is_sel = (i == center_row);

        IswArgBuilder ab = IswArgBuilderInit();
        IswArgLabel(&ab, wm->switcher_labels[idx]);
        if (scheme) {
            if (is_sel) {
                IswArgBackground(&ab, (Pixel)scheme->active);
                IswArgForeground(&ab, (Pixel)scheme->fg_light);
            } else {
                IswArgBackground(&ab, (Pixel)scheme->bg);
                IswArgForeground(&ab, (Pixel)scheme->fg_light);
            }
        }
        IswSetValues(cw->composite.children[i], ab.args, ab.count);
    }
}

/* Create the OSD shell and label widgets (called once per activation) */
static void create_osd(Wm *wm)
{
    const IsdeColorScheme *scheme = isde_theme_current();

    double sf = ISWScaleFactor(wm->toplevel);
    int log_sw = (int)(wm->screen->width_in_pixels / sf + 0.5);
    int log_sh = (int)(wm->screen->height_in_pixels / sf + 0.5);

    int max_height = log_sh / 3;
    int row_h = isde_font_height("general", 4);

    int max_rows = (max_height - 2 * SWITCHER_PAD) / row_h;
    if (max_rows < 1) max_rows = 1;
    int visible = wm->switcher_count;
    if (visible > max_rows)
        visible = max_rows;
    wm->switcher_visible = visible;

    int osd_h = visible * row_h + 2 * SWITCHER_PAD;
    int osd_w = SWITCHER_WIDTH;
    int sx = (log_sw - osd_w) / 2;
    int sy = (log_sh - osd_h) / 2;

    IswArgBuilder ab = IswArgBuilderInit();
    IswArgX(&ab, sx);
    IswArgY(&ab, sy);
    IswArgWidth(&ab, osd_w);
    IswArgHeight(&ab, osd_h);
    IswArgOverrideRedirect(&ab, True);
    IswArgBorderWidth(&ab, 1);
    if (scheme) {
        IswArgBorderColor(&ab, (Pixel)scheme->border);
        IswArgBackground(&ab, (Pixel)scheme->bg);
    }
    wm->switcher_shell = IswCreatePopupShell("switcherOSD",
                                             overrideShellWidgetClass,
                                             wm->toplevel, ab.args, ab.count);

    int label_w = osd_w - 2 * SWITCHER_PAD;

    /* Create placeholder labels — content set by update_labels() */
    for (int i = 0; i < visible; i++) {
        IswArgBuilderReset(&ab);
        IswArgLabel(&ab, "");
        IswArgWidth(&ab, label_w);
        IswArgHeight(&ab, row_h);
        IswArgBorderWidth(&ab, 0);
        IswArgJustify(&ab, IswJustifyLeft);
        IswArgInternalWidth(&ab, 4);
        if (scheme) {
            IswArgBackground(&ab, (Pixel)scheme->bg);
            IswArgForeground(&ab, (Pixel)scheme->fg_light);
        }
        IswCreateManagedWidget("switcherItem", labelWidgetClass,
                               wm->switcher_shell, ab.args, ab.count);
    }

    IswRealizeWidget(wm->switcher_shell);

    /* Position each label */
    CompositeWidget cw = (CompositeWidget)wm->switcher_shell;
    for (int i = 0; i < visible && i < (int)cw->composite.num_children; i++) {
        IswConfigureWidget(cw->composite.children[i],
                          SWITCHER_PAD,
                          SWITCHER_PAD + i * row_h,
                          label_w, row_h, 0);
    }

    update_labels(wm);
    IswPopup(wm->switcher_shell, IswGrabNone);
}

void wm_switcher_show(Wm *wm)
{
    if (wm->switcher_active) {
        wm_switcher_next(wm);
        return;
    }

    build_order(wm);
    if (wm->switcher_count < 2) {
        free(wm->switcher_order);
        wm->switcher_order = NULL;
        free(wm->switcher_labels);
        wm->switcher_labels = NULL;
        wm->switcher_count = 0;
        return;
    }

    xcb_grab_keyboard_cookie_t ck =
        xcb_grab_keyboard(wm->conn, 1, wm->root, XCB_CURRENT_TIME,
                          XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    xcb_grab_keyboard_reply_t *reply = xcb_grab_keyboard_reply(wm->conn, ck, NULL);
    if (!reply || reply->status != XCB_GRAB_STATUS_SUCCESS) {
        free(reply);
        free(wm->switcher_order);
        wm->switcher_order = NULL;
        free(wm->switcher_labels);
        wm->switcher_labels = NULL;
        wm->switcher_count = 0;
        return;
    }
    free(reply);

    wm->switcher_active = 1;
    wm->switcher_sel = 1;

    create_osd(wm);
}

void wm_switcher_next(Wm *wm)
{
    if (!wm->switcher_active || wm->switcher_count == 0) return;

    wm->switcher_sel++;
    if (wm->switcher_sel >= wm->switcher_count)
        wm->switcher_sel = 0;

    update_labels(wm);
}

void wm_switcher_prev(Wm *wm)
{
    if (!wm->switcher_active || wm->switcher_count == 0) return;

    wm->switcher_sel--;
    if (wm->switcher_sel < 0)
        wm->switcher_sel = wm->switcher_count - 1;

    update_labels(wm);
}

void wm_switcher_commit(Wm *wm)
{
    if (!wm->switcher_active) return;

    WmClient *target = NULL;
    if (wm->switcher_sel >= 0 && wm->switcher_sel < wm->switcher_count)
        target = wm->switcher_order[wm->switcher_sel];

    xcb_ungrab_keyboard(wm->conn, XCB_CURRENT_TIME);
    destroy_osd(wm);
    wm->switcher_active = 0;

    if (target) {
        if (target->minimized)
            wm_restore_client(wm, target);
        wm_focus_client(wm, target);
    }
}

void wm_switcher_cancel(Wm *wm)
{
    if (!wm->switcher_active) return;

    xcb_ungrab_keyboard(wm->conn, XCB_CURRENT_TIME);
    destroy_osd(wm);
    wm->switcher_active = 0;
}
