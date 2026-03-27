/*
 * isde-settings-plugin.h — plugin interface for isde-settings panels
 *
 * Core panels are compiled into isde-settings.
 * Third-party panels are .so files loaded via dlopen from:
 *   $XDG_DATA_HOME/isde/settings-plugins/
 *   $XDG_DATA_DIRS/isde/settings-plugins/
 *
 * Each .so must export a function named "isde_settings_panel"
 * that returns a pointer to a static IsdeSettingsPanel struct.
 */
#ifndef ISDE_SETTINGS_PLUGIN_H
#define ISDE_SETTINGS_PLUGIN_H

#include <X11/Intrinsic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IsdeSettingsPanel {
    const char *name;       /* Display name: "Appearance" */
    const char *icon;       /* Icon name (resolved via isde_icon_find) or NULL */
    const char *section;    /* Config section this panel edits: "appearance" */

    /* Create the panel's widget tree inside parent.
     * Do NOT create viewports or Save/Revert buttons — the shell
     * provides a scrollable container and common buttons.
     * Returns the top-level container widget (typically a Form). */
    Widget (*create)(Widget parent, XtAppContext app);

    /* Save current settings to config and apply. */
    void (*apply)(void);

    /* Revert to last saved values. */
    void (*revert)(void);

    /* Returns 1 if the panel has unsaved changes. */
    int (*has_changes)(void);

    /* Cleanup when the panel is destroyed. */
    void (*destroy)(void);
} IsdeSettingsPanel;

/* Symbol name exported by plugin .so files */
#define ISDE_SETTINGS_PANEL_SYMBOL "isde_settings_panel"

/* Plugin entry point type */
typedef const IsdeSettingsPanel *(*IsdeSettingsPanelFunc)(void);

#ifdef __cplusplus
}
#endif

#endif /* ISDE_SETTINGS_PLUGIN_H */
