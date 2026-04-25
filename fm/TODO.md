# isde-fm — Feature TODO

## Context Menu Enhancements

- ~~**Custom script actions**~~ Done — scans `~/.config/isde/fm-actions/` and `$XDG_DATA_DIRS/isde/fm-actions/` for executable scripts with optional `.desktop` companion files (`Name=`, `MimeType=`, `FilePattern=`). User dir takes priority. Scripts receive selected file paths as argv.

## Selection & Navigation

- **Inline rename** — click on an already-selected item's label to enter edit mode. Overlay an AsciiText widget on the label position. Commit on Enter, cancel on Escape.
- **Type-ahead find** — start typing to jump to matching filename

## File Operations

- **Undo** — maintain a stack of recent operations (copy, move, rename, delete) with undo support. Deleted files go to a trash directory first.

## Views

- **Thumbnail previews** — for image files, load and display a thumbnail instead of the generic icon

## Integration

- **Desktop icon management** — manage icons on the root window (`_NET_WM_WINDOW_TYPE_DESKTOP`), read `~/Desktop/*.desktop`
