/*
 * isde-tray.h — helpers for system tray applet popups
 */
#ifndef ISDE_TRAY_H
#define ISDE_TRAY_H

#include <ISW/Intrinsic.h>
#include <ISW/IswTrayIcon.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Position a popup shell above a tray icon, aligned to the panel edge.
 *
 * Uses _NET_WORKAREA to determine the panel boundary so that the
 * popup's bottom edge sits flush against the panel top — regardless of
 * how far the icon is inset within the panel.
 *
 * Parameters:
 *   toplevel    - The application's toplevel shell (for connection/screen)
 *   tray_icon   - The IswTrayIcon handle (for icon X position)
 *   popup_shell - The OverrideShell to position
 */
void isde_tray_position_popup(Widget toplevel, IswTrayIcon tray_icon,
                              Widget popup_shell);

#ifdef __cplusplus
}
#endif

#endif /* ISDE_TRAY_H */
