#define _POSIX_C_SOURCE 200809L
#include "term.h"

#include <libtsm.h>

#include <stdlib.h>
#include <string.h>

/* Thin wrappers so term-widget.c doesn't need to include libtsm.h directly;
 * kept in a separate TU to keep compile units small. The widget code calls
 * these to operate on the screen/vte. */

int term_tsm_new(struct tsm_screen **screen_out, struct tsm_vte **vte_out,
                 tsm_vte_write_cb write_cb, void *user)
{
    if (tsm_screen_new(screen_out, NULL, NULL) < 0) return -1;
    if (tsm_vte_new(vte_out, *screen_out, write_cb, user, NULL, NULL) < 0) {
        tsm_screen_unref(*screen_out);
        *screen_out = NULL;
        return -1;
    }
    return 0;
}

void term_tsm_apply_palette(struct tsm_vte *vte, const TermPalette *pal)
{
    uint8_t colors[TSM_COLOR_NUM][3];
    memset(colors, 0, sizeof(colors));
    for (int i = 0; i < 16; i++) {
        colors[i][0] = pal->rgb[i][0];
        colors[i][1] = pal->rgb[i][1];
        colors[i][2] = pal->rgb[i][2];
    }
    colors[TSM_COLOR_FOREGROUND][0] = pal->rgb[16][0];
    colors[TSM_COLOR_FOREGROUND][1] = pal->rgb[16][1];
    colors[TSM_COLOR_FOREGROUND][2] = pal->rgb[16][2];
    colors[TSM_COLOR_BACKGROUND][0] = pal->rgb[17][0];
    colors[TSM_COLOR_BACKGROUND][1] = pal->rgb[17][1];
    colors[TSM_COLOR_BACKGROUND][2] = pal->rgb[17][2];
    tsm_vte_set_custom_palette(vte, colors);
    tsm_vte_set_palette(vte, "custom");
}
