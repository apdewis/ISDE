/*
 * screenshot.h — isde-screenshot shared types and declarations
 */

#ifndef ISDE_SCREENSHOT_H
#define ISDE_SCREENSHOT_H

#include <stdint.h>
#include <ISW/Intrinsic.h>

typedef struct {
    uint8_t        *rgba;
    unsigned int    width;
    unsigned int    height;
} Screenshot;

/* capture.c */
Screenshot *capture_fullscreen(xcb_connection_t *conn, xcb_screen_t *screen);
Screenshot *capture_area(xcb_connection_t *conn, xcb_screen_t *screen);
void        screenshot_free(Screenshot *ss);

/* preview.c */
Widget preview_create(Widget parent, char *name);
void   preview_set_image(Widget preview, const Screenshot *ss);

/* save.c */
int  save_png(const Screenshot *ss, const char *path);
void save_dialog(Widget parent, const Screenshot *ss);

#endif /* ISDE_SCREENSHOT_H */
