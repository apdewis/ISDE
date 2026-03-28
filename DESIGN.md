# ISDE Design Guide

UI and interaction conventions for all ISDE components. Follow these rules when building or modifying any user-facing interface.

## Pane Toning

- **Navigation/content split**: When a UI has adjacent panes in a navigation/content pattern (e.g. category sidebar + item list, tree view + detail area), the navigation pane uses `scheme->bg` (darker) and the content pane uses `scheme->bg_light` (lighter). The difference should be subtle — just enough to visually separate the two areas.

## Scrolling

- **Scrollbar placement**: Scrollbars go on the right side (`XtNuseRight, True`). Never place scrollbars on the left unless the context specifically requires it.
