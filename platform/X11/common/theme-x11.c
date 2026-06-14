#define _POSIX_C_SOURCE 200809L
/*
 * theme-x11.c — X11-specific theme publishing.
 *
 * The theme payload (colours, fonts, cursor) is built platform-agnostically by
 * isde_theme_build_resource_string() in platform/common/isde-theme.c.  Writing
 * it to the root window's RESOURCE_MANAGER atom is X11-specific and lives here.
 */
#include "isde-theme.h"

#include <stdlib.h>
#include <string.h>
#include <ISW/ISWPlatform.h>

void isde_theme_set_resource_manager(IswDisplay dpy)
{
    if (!dpy) return;

    char *rdb = isde_theme_build_resource_string();
    if (!rdb) return;

    Atom rm = _IswPlatformInternAtomOp(dpy, "RESOURCE_MANAGER", False);
    if (rm != None) {
        _IswPlatformChangeProperty(dpy, _IswDefaultRootWindow(dpy),
                                   rm, ISW_ATOM_STRING, 8,
                                   ISW_PROP_MODE_REPLACE,
                                   rdb, (uint32_t)strlen(rdb));
        _IswPlatformFlush(dpy);
    }

    free(rdb);
}
