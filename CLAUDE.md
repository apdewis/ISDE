# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ISDE (Infi Systems Desktop Environment) is a modular, lightweight X11 desktop environment modeled after LXDE's architecture. Each component is an independent, replaceable program that can function standalone or as part of the full desktop stack.

### Core Components

| Binary | Directory | Role |
|---|---|---|
| `libisde.so` | `libisde/` | Shared utility library: TOML config parser, XDG helpers, EWMH atom cache, .desktop parser |
| `isde-session` | `session/` | Session manager: reads `isde.toml`, starts WM + autostart entries, respawns crashed children |
| `isde-wm` | `wm/` | Window manager: frame decoration, focus, move/resize, EWMH/ICCCM, virtual desktops |
| `isde-panel` | `panel/` | Panel: taskbar, app menu (.desktop files), system tray (XEmbed), clock |
| `isde-fm` | `fm/` | File manager: icon/list views, navigation, file ops, desktop icons, MIME associations |
| `isde-term` | `term/` | Terminal emulator: VT100/xterm, PTY, scrollback, selection |
| `isde-settings` | `settings/` | Settings UI: appearance, display (xrandr), input devices |
| — | `common/` | Default configs, `isde.desktop` session file for display managers (no compiled code) |

Each component is a separate binary. Components communicate via EWMH root window properties, X ClientMessage, X Selections, and the freedesktop System Tray Protocol. No custom IPC daemons — all standard X11 mechanisms.

### Build Commands

```sh
cmake -B build        # configure
cmake --build build -j$(nproc)  # build all components
```

All components link against libISW via `pkg_check_modules(ISW REQUIRED isw)`. The top-level `CMakeLists.txt` finds all dependencies once; each component subdirectory adds its own target.

## Primary Dependency: libISW

ISDE is built on **libISW** (Infi Systems Widgets), a pure-XCB widget toolkit forked from Xaw3d. Reference checkout at `~/libISW`.

Key characteristics of libISW:
- Written in C; provides both Xt Intrinsics and widget set in a single `libISW.so`
- Pure XCB — zero Xlib dependency
- Cairo-XCB rendering backend (mandatory), optional EGL acceleration
- Widgets: Box, Form, Paned, Viewport, MainWindow, Dialog, Label, Command, Toggle, List, Tree, Tabs, Text, Scrollbar, ProgressBar, MenuBar, MenuButton, Image, SimpleMenu/SmeBSB, IconView, SpinBox, Slider, StatusBar, ColorPicker, FontChooser, and more
- XDND drag-and-drop support: `ISWXdndEnable()` / `ISWXdndWidgetAcceptDrops()` (drop targets, protocol v5)
- Public headers in `<ISW/*.h>` (widgets, rendering, SVG) and `<X11/*.h>` (Xt Intrinsics)
- Rendering API in `ISWRender.h`: drawing primitives, 3D shadows, text, clipping, HiDPI scaling, gradients
- Xt programming model: `XtAppInitialize()`, `XtCreateManagedWidget()`, `XtSetArg()`/`XtSetValues()`, callbacks, `XtAppMainLoop()`
- Reference demo: `~/libISW/examples/isw_demo.c`

Linking: `pkg-config --cflags --libs isw`

## Build & System Dependencies

All ISDE components are C targeting XCB/X11. Build tooling uses **CMake** (>= 3.16). Dependencies are found via `pkg-config` / `PkgConfig` CMake module.

Required system libraries:
- `libISW` (the widget toolkit)
- `libxcb` and extensions: xcb-xfixes, xcb-render, xcb-shape, xcb-xrm, xcb-keysyms, xcb-util
- `cairo`, `cairo-xcb` (>= 1.12.0)
- `libxft`, `fontconfig`, `freetype2`
- `libsm`, `libice` (X Session Management)
- Build tools: `gcc`, `cmake` (>= 3.16), `pkg-config`

## Design Principles

- **Modularity** — every component runs independently; no component requires the full DE to function
- **Lightweight** — C only, minimal dependencies, low memory footprint
- **Replaceable** — any component can be swapped for an alternative (e.g. use a different window manager)
- **Standards-compliant** — freedesktop.org specs (XDG dirs, .desktop files, EWMH/ICCCM for WM)

## Architecture Notes

- **isde-wm** cannot use Xt's main loop for core WM logic (needs SubstructureRedirect on root window for all clients). Use pure XCB event loop for WM core; ISW widgets only for WM's own UI (e.g. Alt+Tab dialog).
- **IPC** is entirely X11-based: EWMH properties + PropertyNotify for state broadcast, X ClientMessage for commands, X Selections for clipboard, `_NET_SYSTEM_TRAY` + XEmbed for tray icons, `_NET_WM_STRUT_PARTIAL` for panel space reservation.
- **Config reload**: settings manager writes XDG config files; other components re-read on `SIGHUP` or via inotify.
- **Session autostart**: `@`-prefixed entries in the autostart file are respawned on crash (matching LXDE's lxsession convention).

## ISW Coding Rules

- **Arg arrays**: Always declare `Arg args[20]` or larger. Never size Arg arrays to the exact count — resources get added over time and undersized arrays cause stack smashing. 20 is a safe default.
- **ISW is ASCII only**: Do not use Unicode characters for widget labels. Use SVG icons via `XtNimage`.
- **List/IconView data lifetime**: These widgets store pointers to the label/icon arrays, they do not copy. The backing arrays must remain valid for the lifetime of the widget or until replaced by a new `IswListChange`/`IswIconViewSetItems` call. Clear the widget (set 0 items) before freeing the backing data.
- **Shell geometry**: `XtSetValues` on shell widgets for x/y/width/height may be ignored by geometry management. Use `XtConfigureWidget` from `<X11/IntrinsicP.h>` for reliable positioning.
- **No fork/exec for system operations**: Never fork/exec external tools (cp, rm, xset, xrandr, etc.) when library or system calls exist. Use POSIX APIs for file operations, XCB APIs for X11 settings, xcb-randr for display configuration. Forking to launch user applications (e.g. opening a file with its associated app) is acceptable.
- **do not directly alter libISW** If changes are required there then supply that information, it is not your job to modify external dependencies.

## Design Guide

See [DESIGN.md](DESIGN.md) for HIG rules — scrollbar placement, widget conventions, and other UI standards. Follow it when creating or modifying any user-facing interface.

## Communication Style

- When corrected, just fix the problem. No "you're right" or other glad-handling — be direct. Don't agree with corrections that are actually wrong either; push back when warranted.
- If you discover related problems, stop and communicate about it instead of going on tangets making additional changes
