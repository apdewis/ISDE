/*
 * isde-dbus.h — D-Bus settings notification service
 *
 * Uses libdbus-1 to provide a settings change notification bus.
 * Components subscribe to SettingsChanged signals to know when
 * to reload their config files.
 *
 * If D-Bus is unavailable at runtime, all functions are no-ops
 * and callers should fall back to X11 IPC (ISDE_CMD_RELOAD).
 */
#ifndef ISDE_DBUS_H
#define ISDE_DBUS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IsdeDBus IsdeDBus;

/* D-Bus interface constants */
#define ISDE_DBUS_SERVICE    "org.isde.Settings"
#define ISDE_DBUS_PATH       "/org/isde/Settings"
#define ISDE_DBUS_INTERFACE  "org.isde.Settings"
#define ISDE_DBUS_SIGNAL     "SettingsChanged"

/* Connect to the session bus.  Returns NULL if D-Bus is unavailable. */
IsdeDBus *isde_dbus_init(void);
void      isde_dbus_free(IsdeDBus *bus);

/* Get the file descriptor for event loop integration.
 * Use with IswAppAddInput or poll().  Returns -1 if not connected. */
int       isde_dbus_get_fd(IsdeDBus *bus);

/* Process pending D-Bus messages.  Call when the fd is readable. */
void      isde_dbus_dispatch(IsdeDBus *bus);

/* --- Settings service (used by isde-settings) --- */

/* Emit a SettingsChanged signal.
 * section: TOML section name (e.g. "input", "panel.clock")
 * key: specific key, or "*" for full-section reload */
void      isde_dbus_settings_notify(IsdeDBus *bus,
                                     const char *section,
                                     const char *key);

/* --- Settings client (used by all components) --- */

typedef void (*IsdeSettingsChangedCb)(const char *section,
                                      const char *key,
                                      void *user_data);

/* Register a callback for SettingsChanged signals.
 * Multiple callbacks can be registered. */
void      isde_dbus_settings_subscribe(IsdeDBus *bus,
                                        IsdeSettingsChangedCb cb,
                                        void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* ISDE_DBUS_H */
