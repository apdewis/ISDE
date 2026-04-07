# ISDE — TODO

## Display Manager

### Architecture: Daemon/Greeter Split

Two binaries following the standard DM pattern (greetd, LightDM, GDM):

- **`isde-dm`** — Privileged daemon (runs as root). Manages seats via libseat, authenticates via PAM, starts X servers, launches sessions, handles VT switching. Exposes a D-Bus system bus interface. No GUI libraries. Directory: `dm/`
- **`isde-greeter`** — Unprivileged greeter (runs as `isde-dm` user). ISW/Xt application rendering login screen, lock screen. Communicates with `isde-dm` over a Unix socket. Directory: `dm/` (same CMake subdirectory, two targets)

### isde-dm Daemon

**Source files:** `dm/src/main.c`, `seat.c`, `auth.c`, `session.c`, `greeter.c`, `ipc.c`, `dbus.c`, `power.c`, `platform.h`, `platform_linux.c`, `platform_freebsd.c` — only one `platform_*.c` is compiled per OS (see "OS Portability Strategy" section). All platform-specific logic lives in the vtable implementation; mainline files call through `dm_platform_ops()`.

**Main loop:** `poll(2)` on libseat fd, greeter IPC socket fd, D-Bus system bus fd, signal self-pipe fd. No Xt/ISW.

**Dependencies:** libseat, libpam, libdbus-1, libxcb (for Xauthority cookie generation only)

**Startup sequence:**
1. Acquire seat via `libseat_open_seat()`
2. Generate random Xauthority cookie, write to `<rundir>/Xauthority`
3. Start X server: `Xorg :0 vtN -auth <rundir>/Xauthority -nolisten tcp` (VT from libseat)
4. Wait for X readiness (poll display socket)
5. Fork `isde-greeter` as `isde-dm` user with `DISPLAY` and `XAUTHORITY` set
6. Listen on IPC socket for greeter requests
7. On successful auth: `pam_open_session`, `setuid`/`setgid` to target user, exec selected session
8. When session exits: restart greeter (back to step 5)

### isde-greeter UI

**Source files:** `dm/src/greeter/main.c`, `greeter.c`, `greeter.h`, `clock.c`, `sessions.c`, `ipc.c`

**Widgets:** Form, Label, Command, AsciiText (`XtNecho False` for password), MenuButton (session selector), Image (DE logo), OverrideShell (fullscreen)

**Event loop:** `XtAppMainLoop` with `XtAppAddInput()` on the daemon IPC socket fd.

#### Login Screen

Fullscreen OverrideShell, `scheme->bg` background. Content centered.

- Clock (large, top-center) + date below — timer pattern from `panel/src/clock.c`, `XtAppAddTimeOut`, configurable format
- Username label + AsciiText input (`scheme->bg_bright`)
- Password label + AsciiText input (`XtNecho False`, `scheme->bg_bright`)
- Session label + MenuButton dropdown (`scheme->bg_bright`) — populated from `/usr/share/xsessions/*.desktop` via `isde_desktop_scan_dir()`
- Error message Label (`scheme->error` color, initially empty)
- "Log In" button
- Bottom row: "Shut Down", "Reboot", "Suspend" buttons (visibility controlled by `[dm]` config)
- Keyboard: Enter in password triggers login, Tab moves between fields, Escape clears error

#### Lock Screen

Same widget tree as login, differences:
- Username pre-filled and read-only (`XtSetSensitive(userText, False)`)
- Password field focused
- "Switch User" button visible (sends `SWITCH_USER` to daemon, which starts a new greeter on a new VT)
- Keyboard/pointer grabbed via `xcb_grab_keyboard()` / `xcb_grab_pointer()`

Triggered by daemon sending `MODE_LOCK <username>` to greeter.

### Confirmation Panel (Session-Side)

The logout/shutdown/reboot confirmation panel runs within the user's X session as a fullscreen override-redirect window, not on the greeter VT. This avoids VT switch latency for a transient dialog.

Implementation: a small helper binary or a mode of isde-session that creates an OverrideShell with:
- Centered Form (~`isde_scale(350)` x `isde_scale(150)`)
- Label: "Are you sure you want to shut down?" / "...log out?" / "...reboot?"
- Button row (bottom-right per HIG): affirmative action first ("Shut Down"), "Cancel" last
- Escape or Cancel dismisses
- Confirm sends the command to the DM daemon via D-Bus

### IPC Protocol (daemon <-> greeter)

Unix domain socket at `<rundir>/greeter.sock` (owned root:isde-dm, mode 0660). Line-based request/response.

**Greeter to Daemon:**
- `AUTH <username> <password>` — authenticate
- `SESSION <desktop-file-name>` — set desired session
- `SHUTDOWN` / `REBOOT` / `SUSPEND` — power actions
- `SWITCH_USER` — start new greeter on new VT
- `CANCEL_LOCK` — unlock after successful auth
- `LIST_SESSIONS` — request session list

**Daemon to Greeter:**
- `AUTH_OK` — success, session starting
- `AUTH_FAIL <message>` — PAM error string
- `SESSIONS <count>\n<name>\t<file>\n...` — session list
- `MODE_LOGIN` — switch to login mode
- `MODE_LOCK <username>` — switch to lock mode

### D-Bus Interface (system bus)

**Bus name:** `org.isde.DisplayManager`
**Object path:** `/org/isde/DisplayManager`
**Interface:** `org.isde.DisplayManager`

**Methods:**
- `Lock()` — lock current session (switch to greeter VT, send `MODE_LOCK`)
- `SwitchUser()` — show login on new VT, keep current session
- `Shutdown()` / `Reboot()` — perform power action
- `Suspend()` — suspend immediately
- `ShowConfirmation(action: string)` — trigger session-side confirmation panel

**Signals:**
- `SessionStarted(username, session)` / `SessionEnded(username)`
- `Locked()` / `Unlocked()`

**Policy:** D-Bus policy file restricts methods to `isde-dm` group and active session user. No PolicyKit required.

### PAM Integration

**Service file:** `/etc/pam.d/isde-dm`
```
auth       required   pam_unix.so
account    required   pam_unix.so
session    required   pam_unix.so
password   required   pam_unix.so
```

**Auth flow in `auth.c`:**
1. `pam_start("isde-dm", username, &conv, &handle)` — conv function answers prompts with IPC-received password
2. `pam_authenticate(handle, 0)`
3. `pam_acct_mgmt(handle, 0)`
4. On success: `pam_setcred(handle, PAM_ESTABLISH_CRED)`, `pam_open_session(handle, 0)`
5. `pam_getenvlist()` into session environment
6. On session end: `pam_close_session()`, `pam_end()`
7. Password buffers zeroed immediately after PAM conversation

### Seat Management (libseat)

Required from day one. `isde-dm` uses libseat for:
- **Seat acquisition:** `libseat_open_seat()` on startup, connects to seatd
- **VT switching:** `libseat_switch_session()` for lock/unlock and user switching
- **Seat enable/disable:** `libseat_disable_seat()` before suspend, `libseat_enable_seat()` on resume
- **Device access:** seatd manages DRM/input device access; daemon passes seat fd to X server and session via fd inheritance

**VT allocation:** greeter gets VT 1 (or first available from libseat). User sessions get subsequent VTs. Daemon tracks VT-to-session mappings.

### Power Management

Power operations are dispatched through the platform vtable (`dm_platform_ops()`). `power.c` contains only platform-agnostic logic (SIGTERM/SIGKILL session processes, libseat disable/enable around suspend). The actual system calls live in the platform files.

**Linux (`platform_linux.c`):**
- **Shutdown/Reboot:** `reboot(2)` with `RB_POWER_OFF` / `RB_AUTOBOOT`. Before calling: `sync()`.
- **Suspend:** Write `"mem"` to `/sys/power/state`.
- **Fallback:** D-Bus calls to `org.freedesktop.login1.Manager.PowerOff()` / `.Reboot()` / `.Suspend()` (elogind/systemd-logind).

**FreeBSD (`platform_freebsd.c`):**
- **Shutdown/Reboot:** `reboot(RB_POWEROFF)` / `reboot(RB_AUTOBOOT)` (from `<sys/reboot.h>`, different flag names from Linux).
- **Suspend:** ACPI ioctl on `/dev/acpi` (`ACPIIO_SETSLPSTATE`).
- **Fallback:** D-Bus calls to ConsoleKit2 (`org.freedesktop.ConsoleKit.Manager.Stop()` / `.Restart()` / `.Suspend()`).

Common: `power.c` calls `libseat_disable_seat()` before suspend and `libseat_enable_seat()` on resume. On resume, switch to the greeter VT and trigger lock if `lock_timeout > 0`.

### OS Portability Strategy

Platform-specific code is isolated behind a vtable (ops struct). Each OS provides its own implementation; the daemon selects the correct one at compile time via CMake source selection. No `#ifdef` blocks in mainline daemon code.

**Ops struct (`platform.h`):**
```c
typedef struct DmPlatformOps {
    /* Power management */
    int  (*shutdown)(void);
    int  (*reboot)(void);
    int  (*suspend)(void);

    /* VT management (fallback when libseat is insufficient) */
    int  (*vt_activate)(int vt);
    int  (*vt_wait_active)(int vt);
    const char *(*vt_device_path)(int vt);  /* returns static string */

    /* Paths */
    const char *rundir;          /* "/run/isde-dm" or "/var/run/isde-dm" */
    const char *pam_service_dir; /* "/etc/pam.d" or "/usr/local/etc/pam.d" */
} DmPlatformOps;

/* Returns the ops table for the current platform. */
const DmPlatformOps *dm_platform_ops(void);
```

**Platform source files:**
- `platform.h` — ops struct definition and `dm_platform_ops()` prototype
- `platform_linux.c` — Linux implementation, compiled only on Linux
- `platform_freebsd.c` — FreeBSD implementation, compiled only on FreeBSD

Each platform file defines a static `DmPlatformOps` instance and `dm_platform_ops()` returns a pointer to it. CMake selects which file to compile.

**Daemon code** calls `dm_platform_ops()->shutdown()` etc. — no conditional compilation in `main.c`, `seat.c`, `session.c`, `power.c`, or any other mainline file.

**Platform differences handled by the vtable:**

| Area | Linux | FreeBSD |
|------|-------|---------|
| Shutdown | `reboot(2)` with `RB_POWER_OFF` | `reboot(RB_POWEROFF)` |
| Reboot | `reboot(2)` with `RB_AUTOBOOT` | `reboot(RB_AUTOBOOT)` |
| Suspend | `/sys/power/state` | ACPI ioctl `ACPIIO_SETSLPSTATE` |
| Power fallback | elogind D-Bus | ConsoleKit2 D-Bus |
| VT ioctls | `<linux/vt.h>` | `<sys/consio.h>` |
| VT device paths | `/dev/tty[N]` (1-based decimal) | `/dev/ttyv[N]` (0-based hex) |
| Runtime dir | `/run/isde-dm` | `/var/run/isde-dm` |
| PAM service dir | `/etc/pam.d` | `${CMAKE_INSTALL_SYSCONFDIR}/pam.d` |

**PAM API**: Both Linux-PAM and OpenPAM expose the same `pam_*()` API — no vtable entry needed. Only the install path for the service file differs.

**Adding a new OS**: Implement a new `platform_<os>.c` with the ops struct, add a CMake conditional to compile it. No changes to daemon mainline code.

### Session File Handling

Greeter scans `/usr/share/xsessions/` via `isde_desktop_scan_dir()`:
- `isde_desktop_name()` provides the display name for the session selector
- `isde_desktop_exec()` provides the command to exec
- Accept both `Type=XSession` and `Type=Application`
- Last-used session stored per-user in `<rundir>/users/<username>/last-session`

Update `common/data/isde.desktop`: change `Type=Application` to `Type=XSession`.

### New IPC Commands (isde-ipc.h)

```c
#define ISDE_CMD_LOCK        4  /* Request screen lock */
#define ISDE_CMD_SHUTDOWN    5  /* Request shutdown (with confirmation) */
#define ISDE_CMD_REBOOT      6  /* Request reboot (with confirmation) */
#define ISDE_CMD_SUSPEND     7  /* Request suspend */
```

Session components send these via X ClientMessage. isde-session forwards to the DM daemon via D-Bus.

### Configuration (`/etc/isde/isde-dm.toml`)

The DM has its own system-wide config file, separate from the per-user `isde.toml`. The DM runs as root before any user session exists, so XDG user directories are not applicable.

```toml
# /etc/isde/isde-dm.toml

greeter = "isde-greeter"
xserver = "/usr/bin/Xorg"
default_session = "isde.desktop"
allow_shutdown = true
allow_reboot = true
allow_suspend = true

# Dev mode: Xephyr, no seat, no root required
dev_mode = false

# Phase 2+
# autologin_user = ""
# autologin_session = "isde.desktop"
# lock_timeout = 300

[clock]
time_format = "%H:%M"
date_format = "%Y-%m-%d"

# Phase 4
# [appearance]
# color_scheme = "default-dark"
# background = ""
```

### Build Integration

Add to top-level `CMakeLists.txt`:
- `add_subdirectory(dm)`
- `pkg_check_modules(PAM REQUIRED pam)`
- `pkg_check_modules(LIBSEAT REQUIRED libseat)`

`dm/CMakeLists.txt` produces two targets:
- `isde-dm`: links libpam, libseat, libdbus-1, libxcb
- `isde-greeter`: links isde, ISW, XCB, cairo-xcb (same pattern as isde-panel)

**Platform source selection in CMake:**
```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(DM_PLATFORM_SRC src/platform_linux.c)
    set(DM_PAM_DIR "/etc/pam.d" CACHE PATH "PAM service directory")
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(DM_PLATFORM_SRC src/platform_freebsd.c)
    target_link_libraries(isde-dm PRIVATE util)
    set(DM_PAM_DIR "${CMAKE_INSTALL_SYSCONFDIR}/pam.d" CACHE PATH "PAM service directory")
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

target_sources(isde-dm PRIVATE ${DM_PLATFORM_SRC})
```

No `#ifdef` blocks in mainline code — the platform vtable file is the only OS-specific compilation unit.

Install targets: both binaries to `${CMAKE_INSTALL_BINDIR}`, PAM service to `${DM_PAM_DIR}`, D-Bus policy to `/usr/share/dbus-1/system.d/`

### Security

- Daemon: no GUI libraries, minimal code surface, runs as root
- Greeter: runs as `isde-dm` user, cannot read user home dirs
- Passwords: transit only over local Unix socket (0660 root:isde-dm), zeroed after PAM conversation
- Greeter runs on separate X server from user sessions — no X11 snooping
- Lock security: VT separation + exclusive keyboard/pointer grab on greeter
- D-Bus policy restricts methods to `isde-dm` group and active session user

### Implementation Phases

**Phase 1 — Core login/logout (MVP):**
- `isde-dm` daemon with PAM auth, libseat, X server management, greeter lifecycle
- Platform vtable with Linux and FreeBSD implementations
- `isde-greeter` login screen: username, password, session selector, clock, error display, shutdown/reboot/suspend buttons
- IPC socket protocol
- PAM service file + D-Bus policy file
- CMake build integration with platform source selection

**Phase 2 — Lock screen + D-Bus:**
- D-Bus system bus interface
- Lock screen mode in greeter
- `ISDE_CMD_LOCK` in isde-ipc.h; isde-session forwards to DM via D-Bus
- Idle timeout lock
- isde-wm lock keybinding integration

**Phase 3 — Multi-user + switch user:**
- Multiple simultaneous sessions on different VTs
- Switch User button (new greeter on new VT)
- VT-to-session mapping in daemon

**Phase 4 — Confirmation panel + polish:**
- Session-side confirmation panel (fullscreen override-redirect)
- `ShowConfirmation` D-Bus method
- Autologin support
- Greeter appearance customization, background image
- `[dm.clock]` format configuration

### Relationship to Existing Components

- **isde-session:** Unchanged. DM daemon execs it via session `.desktop` Exec line after auth.
- **isde-panel:** Shutdown/reboot buttons should call `org.isde.DisplayManager` D-Bus methods (phase 2+). `ISDE_CMD_LOGOUT` continues to work for logout.
- **isde-wm:** Lock keybinding sends `ISDE_CMD_LOCK` (phase 2).
- **isde-settings:** Could gain DM config panel in future.
- **libisde:** New IPC command constants added to `isde-ipc.h`.
- **common/data/isde.desktop:** `Type` changed to `XSession`.

## Keybinding settings

Add keybinding management to isde-settings and ensure apps accept user settings externally.

## Extention architecture.

Extensions should be implemented as subprocesses with secure IPC, for each part of ISDE the required RPC or similar calls should be determined.
Including:
 - WM management and compositing hooks. Allow snapping beheviour and similar to be modular
 - Filemanager: previews, context extensions, etc
 - Panel: Allow replacement of standard widgets, enhance or replace task bar, start menu etc, calendar app hook for the clock and other modularity
 - Settings, allow apps and modules to add to the commons settings area

## HIG (DESIGN.md)

The following areas are not yet covered and should be added as the DE matures:

- **Typography & Text** — font size hierarchy (headings, body, captions), capitalization rules (title case vs sentence case for labels, menus, buttons), ellipsis convention ("Save As..." for actions that open further UI), colon usage on form labels
- **Icons** — standard sizes (16/24/32/48), when to use icon-only vs icon+label vs label-only
- **Keyboard & Focus** — tab order expectations, focus indicator style, mnemonics / accelerator key conventions
- **Drag and Drop** — visual feedback during drag, drop target indication, cursor changes
- **Toolbar Conventions** — icon size, text labels, spacing, separator grouping
- **Status Bar** — content layout, when to show a status bar, transient messages vs persistent state
- **Notifications** — toast / notification patterns if applicable
- **Responsive Behaviour** — how layouts adapt when windows are resized beyond the default or down to the minimum
