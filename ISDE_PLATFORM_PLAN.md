# ISDE Platform-Abstraction Layer — Design Plan

Status: **approved architecture, pending implementation scope.** No component code
changed yet.

## Why this exists

libISW 0.9.0 isolated its X11/XCB code behind a platform backend vtable
(`src/platform/X11/ISWPlatform*XCB.c`, selected by `_IswPlatformSelectBackend()`)
and removed XCB types from its public API. ISDE no longer builds against it.

The trigger was a build break, but the underlying problem is the same one libISW
just solved one layer down: **ISDE's own platform-dependent code** — EWMH, RandR,
DPMS, the ISDE IPC command bus, startup-notification, the system-tray-manager
role, RESOURCE_MANAGER publishing, the compositor protocols — is scattered through
the tree as ad-hoc raw-XCB islands. It deserves the same backend-module isolation.

## Scoping principle: do not re-abstract what libISW already abstracts

This is the load-bearing decision. libISW 0.9.0 already exposes a neutral
platform API for **generic windowing primitives**:

| Already provided by libISW (consume, do not re-wrap) | Entry points |
|---|---|
| Window create/destroy/map/configure/reparent | platform window ops |
| Atom intern / name | `_IswPlatformInternAtomOp`, `_IswPlatformGetAtomName` |
| Property change/get/delete | `_IswPlatformChangeProperty`, `_IswPlatformGetProperty` |
| ClientMessage send | `_IswPlatformSendMessage` |
| Selections / clipboard | `IswOwnSelection`, `IswGetSelectionValue`, … |
| Keyboard/pointer grabs, keysyms | `IswGrabKey`, `IswGrabPointer`, input ops |
| Cursors, colors, coordinate translate | cursor/color ops, `IswTranslateCoords` |
| Per-widget WM state | `IswSetWindowState` |
| Resource database (Xrm replacement) | `IswDatabase`, `_IswPlatformResource*` |

ISDE's layer covers only the **desktop-environment protocols libISW does not and
will not provide** — the semantics above the generic primitives:

1. **EWMH WM-state semantics** — client list (+stacking), active window,
   number/current desktop, desktop layout, workarea, per-window type/class/desktop,
   request messages (`_NET_ACTIVE_WINDOW`, `_NET_CLOSE_WINDOW`, …).
2. **Monitor configuration** — enumerate monitors (geometry + primary + at-point)
   and (settings/displayd) set CRTC mode / primary / screen-size, refresh compute,
   output/EDID enumeration, hotplug events.
3. **Display power** — DPMS get/set timeouts.
4. **ISDE IPC command bus** — `_ISDE_COMMAND` ClientMessage broadcast (send+decode).
5. **Startup notification** — `_NET_STARTUP_INFO*` id gen + emit + cancel.
6. **System-tray-manager role** — `_NET_SYSTEM_TRAY_S<n>` + XEmbed (panel side).
   Tray-icon docking (client side) is already in libISW's `IswTrayIcon`.
7. **RESOURCE_MANAGER publishing** — serialize theme to root property (session).
8. **Compositor protocols** — Composite/Damage/Shape (wm-compositor only).
9. **WM core** — SubstructureRedirect, frames, focus, stacking, WM_Sn (wm only).

## What stays where

### A. Behind the new ISDE platform vtable (shared, multi-component)
- **EWMH** (5+ components) — from `isde-ewmh`
- **Monitor query** (6 components) — from `isde-randr`
- **Monitor config** (settings, displayd) — config sub-vtable
- **IPC command bus** (4+) — from `isde-ipc`
- **DPMS** (settings, session/power) — from `isde-dpms`
- **Startup notification** (wm, fm, panel launchers) — X parts of `isde-desktop`
- **RESOURCE_MANAGER publish** (session) — root-property part of `isde-theme`

### B. Component-private, X-only — NOT in the shared vtable
Consumers of the shared layer, but their core stays component-local raw-XCB:
- **WM core** (`wm/`) — SubstructureRedirect, frames, WM_Sn. Pure-XCB by design.
- **Compositor** (`wm-compositor/`) — Composite/Damage/Shape. X-only.
- **System-tray-manager role** (`panel/`) — panel-private today; defer to a later op.
- **Screenshot capture** (`screenshot/`) — `xcb_get_image`. Backend-specific.

### C. Already neutral — route to libISW, delete the ISDE-side X code
- `isde-theme` per-screen Xrm merge / put-line → libISW resource ops.
- `isde-tray` popup positioning → `IswTranslateCoords` + display-query ops.
- `isde-dialog` / `isde-filechooser` action-proc event types → `IswEvent`.
- `isde-dialog` `_NET_WM_STATE_ABOVE` → `IswSetWindowState`.

## The ISDE platform vtable shape

Mirror libISW: one `const` ops table per backend, sub-vtables per category,
selected once at init. ISDE's layer sits **on top of** libISW's neutral ops for
the toolkit-side, and on raw XCB for the daemon-side that has no toolkit.

```
libisde/include/isde/isde-platform.h        /* neutral: opaque handles, ops vtable, dispatch wrappers */
libisde/include/isde/isde-platform-x11.h     /* X11-only: isde_display_from_xcb for daemons */
libisde/platform/X11/
    isde-platform-x11.c    /* backend selection + IsdeDisplay record (isw-or-xcb) */
    isde-ewmh-x11.c        /* EWMH ops      (xcb-ewmh/icccm)  — absorbs old isde-ewmh.c body  */
    isde-display-x11.c     /* monitor query + config (xcb-randr) — absorbs old isde-randr.c body */
    isde-dpms-x11.c        /* DPMS ops      (xcb-dpms)         — absorbs old isde-dpms.c body  */
    isde-ipc-x11.c         /* _ISDE_COMMAND bus               — absorbs old isde-ipc.c body   */
    isde-startup-x11.c     /* _NET_STARTUP_INFO                                               */
    isde-rootprop-x11.c    /* RESOURCE_MANAGER publish                                        */
```

Sub-vtables:

```c
typedef struct {
    const IsdePlatformEwmhOps          *ewmh;     /* client list, active, desktops, workarea, requests */
    const IsdePlatformDisplayOps       *display;  /* monitor enumerate / primary / at-point / refresh  */
    const IsdePlatformDisplayConfigOps *display_config; /* set CRTC/primary/screen-size, EDID, hotplug — settings/displayd */
    const IsdePlatformPowerOps         *power;    /* DPMS timeouts */
    const IsdePlatformIpcOps           *ipc;      /* command bus send/decode */
    const IsdePlatformStartupOps       *startup;  /* startup-notification */
    const IsdePlatformRootOps          *root;     /* RESOURCE_MANAGER + root-window publish */
} IsdePlatformOps;

const IsdePlatformOps *isde_platform(void);   /* selects backend once */
```

### The connection-handle model (the crux)

The abstraction must own the connection, or it isn't one. A component that calls
`xcb_connect` and hands the result to the layer has not been decoupled from X —
it has wrapped X in a vtable. And libISW exposes **no public way** to recover a
raw `xcb_connection_t` from an `IswDisplay` (the `_IswXcbConn` seam lives in
libISW's private source headers, off-limits per CLAUDE.md). Since ISDE's DE
protocols need `xcb-ewmh` / `xcb-randr` / `xcb-dpms` — none of which have a
neutral libISW equivalent — the layer cannot borrow libISW's connection even if
it wanted to.

**Therefore: the ISDE platform layer always owns its own connection.**

```c
IsdeDisplay isde_platform_open(const char *display_name);  /* layer opens + owns the connection */
int         isde_display_event_fd(IsdeDisplay);            /* for poll()/select() */
IsdeEvent  *isde_display_poll_event(IsdeDisplay);          /* opaque neutral event */
void        isde_display_close(IsdeDisplay);
```

One code path for everyone. No `isde_display_from_isw`, no
`isde_display_from_xcb`. No `xcb_connection_t`, `xcb_window_t`, or `xcb_*` event
struct ever reaches a component.

- **Toolkit components** (panel, fm, term, trays, settings UI) — call
  `isde_platform_open()` for their DE-protocol display, AND separately let libISW
  open its own display for widget rendering. Two X connections per process
  (libISW's for widgets, ISDE's for DE protocol). That cost buys a DE-protocol
  API that is fully platform-agnostic: all X11-ness is inside the backend.

- **Display-agnostic daemons** (session, displayd, dm/greeter) — call
  `isde_platform_open()`, drive their loop off `isde_display_event_fd()` /
  `isde_display_poll_event()`. They stop calling `xcb_connect` entirely.

- **X11-native programs** (wm, wm-compositor) — NOT behind the abstraction; they
  are part of the X11 backend's program set. The WM core *is* SubstructureRedirect
  + a raw event loop + root control; the compositor core *is* Composite/Damage/
  Shape. A non-X backend ships a different compositor binary, it does not "port"
  these. They keep raw XCB and their own event loop, and may link libisde to
  publish EWMH *properties* via the ops. They sit *below* the abstraction, not
  *behind* it.

The X11 backend's `IsdeDisplay` is a single internal form: it owns the
`xcb_connection_t` it opened, plus the screen/root it resolved. Every op uses it
directly with `xcb-ewmh`/`xcb-randr`/`xcb-dpms`.

## Resolved design decisions (locked)

1. **Vtable indirection: full, now.** Build the complete `IsdePlatformOps` vtable
   + a single X11 backend, mirroring libISW — even with no second backend yet.

2. **RandR config: split sub-vtable.** Always-present query side for all 6
   display-aware components; a separate `IsdePlatformDisplayConfigOps` (set CRTC /
   primary / screen-size, EDID, hotplug) reached only by `settings` and `displayd`.

3. **The layer always owns its own connection.** `isde_platform_open()` opens an
   `xcb_connection_t` the X11 backend owns, for every caller. No
   `isde_display_from_isw`, no `isde_display_from_xcb` — libISW exposes no public
   `IswDisplay`→connection accessor and xcb-ewmh/randr/dpms have no neutral
   equivalent, so borrowing libISW's connection is impossible anyway.
   - Toolkit components open an ISDE display for DE protocol AND let libISW open
     its own for widgets (two connections per process; the DE-protocol API stays
     fully platform-agnostic).
   - Display-agnostic daemons (session, displayd, dm/greeter) drive their loop off
     `isde_display_event_fd()` and stop calling `xcb_connect`.
   - X11-native programs (wm, wm-compositor) are NOT behind the layer; they are
     the X11 backend's own programs, keep raw XCB + their own event loop, and only
     consume EWMH ops to publish properties.
   **The old raw public entry points of `isde-ewmh` / `isde-randr` / `isde-dpms` /
   `isde-ipc` are deleted** — their bodies move into the X11 backend TUs as op
   implementations. One code path.

4. **Migration order: layer-first.** Build the vtable + X11 backend, then port
   components directly onto it. Longer to first green build; no throwaway
   seam-patching — each component ported once to final shape.

## Non-goals

- Modifying libISW (forbidden by CLAUDE.md).
- Re-abstracting generic windowing primitives libISW already provides.
- Touching WM/compositor core logic beyond their consumption of shared ops.
