# ISDE Design Guide

UI and interaction conventions for all ISDE components. Follow these rules when building or modifying any user-facing interface.

## Pane Toning

- **Navigation/content split**: When a UI has adjacent panes in a navigation/content pattern (e.g. category sidebar + item list, tree view + detail area), the navigation pane uses `scheme->bg` (darker) and the content pane uses `scheme->bg_light` (lighter). The difference should be subtle — just enough to visually separate the two areas.

## Scrolling

- **Scrollbar placement**: Scrollbars go on the right side (`XtNuseRight, True`). Never place scrollbars on the left unless the context specifically requires it.

## Spacing

- **Pane padding**: Side menus (navigation lists, category bars) and content areas must have `isde_scale(8)` padding on all four sides. Use `XtNdefaultDistance` on the containing Form, or explicit `XtNhorizDistance` / `XtNvertDistance` constraints on edge children.

## Form Layout

- **Label placement**: Labels go to the left of their control, on the same row. Never above. Right-align labels to form a clean column edge against the controls. Use a consistent `XtNwidth` for all labels in a form with `XtNjustify, XtJustifyRight`.
- **Navigation menus**: Side menus and category lists are single-column. Items in these menus use section heading style — `sectionHd` widget name for the heading font (general +2pt bold).
- **Control column**: Controls start at the same horizontal position, forming a left-aligned column to the right of the label column.
- **Row spacing**: Use the standard `isde_scale(8)` vertical distance between rows (`XtNvertDistance` or Form `XtNdefaultDistance`).
- **Group spacing**: Visually related controls share the standard row spacing. Use `isde_scale(16)` between unrelated groups or sections.

## Window Conventions

- **Initial size**: Set a sensible default via `XtNwidth` / `XtNheight` on the shell. All sizes must use `isde_scale()`.
- **Minimum size**: Set `XtNminWidth` / `XtNminHeight` WM hints on all resizable top-level windows. The minimum must be large enough to keep the UI usable (no clipped controls or overlapping buttons).
- **Working area limit**: Never set an initial window size that exceeds the available working area. Query the screen geometry and `_NET_WORKAREA` and clamp if needed.
- **Dialogs**: Confirmation and action dialogs (delete, rename, empty trash, font chooser, etc.) must be modal — use `XtGrabExclusive`. Only informational or progress windows use `XtGrabNone`.
- **Dialog sizing**: Dialogs should be just large enough for their content. Do not set dialogs resizable unless they contain a scrollable or variable-size area.

## Menus

- **Menu bar order**: File, Edit, View, then application-specific menus, then Help/About last. Not every menu needs to be present — include only what applies.
- **Context menus**: Group related items with separators. Put the most common action first. Destructive actions (Delete, Empty Trash) go at the bottom, separated from other items.
- **Keyboard accelerators**: Not yet supported. Do not display accelerator hints in menu items.

## Feedback & State

- **Disabled widgets**: Must be visually distinct from enabled widgets. Use `scheme->fg_dim` for disabled text/icons so they are clearly non-interactive.
- **Selection highlight**: Use `scheme->select_bg` / `scheme->select_fg` from the active color scheme. Never hardcode selection colors.
- **Form validation errors**: Display as an inline message next to or below the offending control. Use `scheme->error` for the message text color. Do not use dialogs or popups for field-level validation.
- **Progress indication**: Show a progress bar immediately when an operation begins — no delay threshold. Use determinate progress when the total is known, indeterminate when it is not.

## Color

- **Scheme-driven**: All UI colors must come from the active `IsdeColorScheme`. Never hardcode color values.
- **Semantic colors**: Use the scheme's semantic fields for their intended purpose:
  - `error` — error messages, failed state
  - `warning` — caution indicators
  - `success` — positive confirmation
  - `active` — focused elements, primary actions
  - `accent` — decorative emphasis
- **Text colors**: `fg` for primary text, `fg_dim` for secondary/disabled text, `fg_light` for text on dark backgrounds.
- **Backgrounds**: `bg` for chrome/navigation, `bg_light` for content areas (see Pane Toning), `bg_bright` for text inputs and interactive form controls (menu buttons, combo boxes).
- **Background ordering**: `bg` is always the darkest, `bg_light` the middle, `bg_bright` the lightest — in both light and dark themes. Theme authors must maintain this ordering.
- **Menu buttons in forms**: Use `scheme->bg_bright` background to match text inputs and distinguish them from static form chrome. Popup menus (SimpleMenu) use `scheme->menu.bg` with a `scheme->border` border.

## Dialog & Panel Buttons

- **Button order**: Affirmative action first (leftmost), Cancel/dismiss last (rightmost). E.g. "OK / Cancel", "Save / Revert", "Delete / Cancel".
- **Alignment**: Action buttons sit at the **bottom-right** of the dialog or panel. Anchor with `XtChainRight` / `XtChainBottom`.
- **Button width**: All buttons in an action row must be the same width. Use `isde_scale(80)` as the standard — enough to fit 10 characters comfortably.
- **Button spacing**: `isde_scale(8)` between buttons in an action row (`XtNhorizDistance`).
- **Button padding**: `isde_scale(8)` internal padding on all sides (`XtNinternalHeight`, `XtNinternalWidth`).
- **Destructive actions**: No special button style. The label alone (e.g. "Delete") communicates the action.
