# isde-fm — Feature TODO

## Selection & Navigation

- **Inline rename** — click on an already-selected item's label to enter edit mode. Overlay an AsciiText widget on the label position. Commit on Enter, cancel on Escape.
- **Type-ahead find** — start typing to jump to matching filename

## File Operations

- **Undo** — maintain a stack of recent operations (copy, move, rename, delete) with undo support. Deleted files go to a trash directory first.

## Integration

- **Desktop icon management** — manage icons on the root window (`_NET_WM_WINDOW_TYPE_DESKTOP`), read `~/Desktop/*.desktop`
