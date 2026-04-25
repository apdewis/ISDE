# ISDE — TODO

## Theme resources: merge into Xrm database

`isde_theme_build_resources()` returns strings that are currently passed to `IswAppInitialize` as Xt fallback resources. Fallbacks are only consulted when the resource DB is otherwise empty, so these values silently stop applying if a user ever has `.Xdefaults`, an app-defaults file, or `xrdb`-loaded resources present. Merge each line into the actual Xrm DB via `XrmPutLineResource(XtDatabase(dpy), ...)` after display open, so theme values participate in normal resource lookup and specificity rules regardless of what else is loaded.

## Panel overflow handling

Taskbar and system tray have no overflow handling. When too many task buttons or tray icons exist for the available space, buttons shrink indefinitely (taskbar) or icons overlap/clip (tray). Needs scroll arrows or an overflow menu for both areas, plus a minimum button width for the taskbar.

## Global hotkeys and keybinding settings
- Consider where hotkey management lives, the general approach seems to be that this lives in the WM, but that also couples the other components to isde-wm, however simplifies WM hotkey operations. Need to decide if de-coupling/modularity/portability is more important or not.
- Add keybinding management to isde-settings and ensure apps accept user settings externally.

## Extention architecture.

Extensions should be implemented as subprocesses with secure IPC, for each part of ISDE the required RPC or similar calls should be determined.
Including:
 - WM management and compositing hooks. Allow snapping beheviour and similar to be modular
 - Filemanager: previews, context extensions, etc
 - Panel: Allow replacement of standard widgets, enhance or replace task bar, start menu etc, calendar app hook for the clock and other modularity
 - Settings, allow apps and modules to add to the commons settings area

## Multi-monitor support

ISDE currently treats the entire X screen as a single work area anchored to the RandR primary output. The following are needed for proper multi-monitor support:

- **Per-monitor work areas in WM** — `wm_get_work_area()` computes one rectangle. The WM needs to enumerate outputs via RandR, maintain per-output geometry, and compute per-monitor work areas (screen rect minus applicable struts). Maximize, snap, and fullscreen should target the monitor the window is on, not the combined screen.
- **Per-monitor window placement** — cascade and dialog centering in `placement.c` should place windows on the monitor where the pointer or parent is, clamped to that monitor's work area.
- **EWMH per-monitor properties** — advertise `_NET_WORKAREA` arrays (one per desktop × monitor), `_NET_DESKTOP_GEOMETRY`, and `_NET_WM_FULLSCREEN_MONITORS`.
- **Panel on each monitor** — either spawn a panel instance per output or allow multi-monitor panel configuration. Secondary monitors currently have no panel.
- **Desktop icons per monitor** — `desktop.c` sizes itself to one `_NET_WORKAREA` rectangle. Desktop icons need per-monitor regions so they don't span across bezels or pile onto one screen.
- **Display configuration in settings** — the display settings panel can enumerate outputs but cannot set resolutions, positions, rotation, or enable/disable monitors. Needs full RandR mode setting and a spatial arrangement UI.
- **Per-monitor DPI scaling** — the current global scale factor should become per-output so mixed-DPI setups render correctly.

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
