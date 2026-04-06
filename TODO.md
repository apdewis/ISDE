# ISDE — TODO

## Display manager

This should:
 - Integrate with seatd for session management
 - Login screen with clock, username + password boxes plus shutdown, sleep, switch user buttons
 - Lock screen that is the login screen but with the switch user option present
 - handle the logout/shutdown/etc confirmation fullscreen panel
 - have a d-bus interface for triggering lock, the confirmation panel etc
 - Load available sessions via the desktop/session format

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
