# isde-fm — Feature TODO

## Context Menu Enhancements

- **"Open with" submenu** — scan `mimeapps.list` and `.desktop` files for entries declaring `MimeType=` that matches the selected file's MIME type. Show matching apps as "Open with <app name>" entries. Requires:
  - Add `MimeType=` field parsing to `isde-desktop.h` / `isde-desktop.c`
  - MIME type detection from file extension (basic built-in map) and/or `shared-mime-info` database
  - Parse `~/.config/mimeapps.list` and `/usr/share/applications/mimeapps.list` for default associations

- **Custom script actions** — scan `~/.config/isde/fm-actions/` for executable scripts. Each script has an optional `.desktop`-style companion file specifying:
  - `MimeType=` — which MIME types the action applies to
  - `FilePattern=` — glob patterns (e.g. `*.tar.gz`)
  - `Name=` — display label in the context menu
  - Scripts receive selected file paths as arguments

- **Desktop Entry Actions on associated app** — when right-clicking a file, if the default app has `[Desktop Action]` entries, show those in the context menu (e.g. right-click an HTML file → "Open in New Private Window" from Firefox's actions)

## Selection & Navigation

- **Inline rename** — click on an already-selected item's label to enter edit mode. Overlay an AsciiText widget on the label position. Commit on Enter, cancel on Escape.
- **Type-ahead find** — start typing to jump to matching filename

## File Operations

- **Undo** — maintain a stack of recent operations (copy, move, rename, delete) with undo support. Deleted files go to a trash directory first.


## Views

- **Detail/column list view** — show name, size, date modified, permissions in columns with sortable headers
- **Places sidebar** — KDE-style left panel with sections for quick navigation. Toggle via View menu. List widget inside a Viewport, grouped by section headers (Label widgets):
- **Thumbnail previews** — for image files, load and display a thumbnail instead of the generic icon

## Integration

- **Desktop icon management** — manage icons on the root window (`_NET_WM_WINDOW_TYPE_DESKTOP`), read `~/Desktop/*.desktop`
- **Drag source** — ISW currently only supports XDND drop targets. Adding drag source to IconView would enable drag-and-drop file operations between FM windows and other apps.

## Tray applications
- **Audio controls** - integrate with PipeWire(linux) or FreeBSD equivalent
- **Bluetooth management** - Bluez integration for device management and pairing
- **Clipboard manager** - clipboard manager with history
- **Network manager frontend** - 
