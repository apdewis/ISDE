# ISW Layout Guide

Rules and guidance for using ISW layout containers in ISDE components.

## Layout Containers

ISW provides three layout containers. Choose based on the layout requirement:

| Container | Use When |
|-----------|----------|
| **Box** | Simple stacking of children along one axis (horizontal or vertical). All children get their requested size. No flexible space distribution. |
| **Form** | Relative positioning with constraints (`fromHoriz`, `fromVert`, chain anchors). Good for dialogs, settings panels, and layouts where widgets are positioned relative to each other or to the container edges. |
| **FlexBox** | One axis with flexible space distribution. One or more children absorb remaining space via `flexGrow`. The correct choice when a layout has both fixed-size and stretchy regions (e.g. a panel bar). |

## FlexBox

`#include <ISW/FlexBox.h>` -- `flexBoxWidgetClass`

### Widget Resources

| Resource | Type | Default | Purpose |
|----------|------|---------|---------|
| `orientation` | `IswOrientation` | `XtorientVertical` | Primary axis |
| `spacing` | `Dimension` | 0 | Gap between children |

### Constraint Resources (per-child)

| Resource | Type | Default | Purpose |
|----------|------|---------|---------|
| `flexGrow` | `Int` | 0 | Proportion of leftover space to absorb. 0 = fixed size. |
| `flexBasis` | `Dimension` | 0 | Base size along the primary axis before flex distribution. |
| `flexAlign` | `IswFlexAlign` | `XtflexAlignStretch` | Cross-axis alignment. |

### FlexAlign Values

- `XtflexAlignStart` -- align to start of cross axis
- `XtflexAlignEnd` -- align to end of cross axis
- `XtflexAlignCenter` -- center on cross axis
- `XtflexAlignStretch` -- stretch to fill cross axis

### Dynamic Resizing

To change a FlexBox child's size at runtime, set `flexBasis` via `IswSetValues`. The FlexBox will relayout all children, redistributing space among `flexGrow` siblings. Do **not** use `IswResizeWidget` or `IswConfigureWidget` on FlexBox children -- the FlexBox will override those on the next layout pass.

### When to Use FlexBox

- A bar or strip with one stretchy region and fixed-size endpoints (e.g. panel: start button | **taskbar** | tray | clock).
- Any layout where one child should absorb all remaining space.
- Layouts that need children to dynamically resize at runtime while other children adjust.

### When NOT to Use FlexBox

- Complex relative positioning (use Form).
- Simple uniform stacking with no flexible regions (use Box).
- Nesting: avoid FlexBox inside FlexBox. If a FlexBox child needs internal layout (e.g. two stacked labels), use a Form or Box as the child container.

## Form

`#include <ISW/Form.h>` -- `formWidgetClass`

### Key Constraint Resources

| Resource | Purpose |
|----------|---------|
| `fromHoriz` | Position this widget to the right of the named sibling. |
| `fromVert` | Position this widget below the named sibling. |
| `top/bottom/left/right` | Chain constraints (`IswChainTop`, `IswChainBottom`, `IswChainLeft`, `IswChainRight`, `IswRubber`). Control how the widget moves/stretches when the Form resizes. |
| `horizDistance` / `vertDistance` | Spacing from the `fromHoriz`/`fromVert` sibling. |
| `resizable` | `Boolean`, default **False**. Must be True for the Form to honor runtime geometry requests from this child. |

### Critical Form Gotchas

1. **`resizable` defaults to False.** `IswSetValues` on a child's width/height will be silently rejected by the Form's GeometryManager unless the child has `IswNresizable, True`.

2. **Width of 1 is magic.** The Form treats `core.width == 1` (or `core.height == 1`) as "use virtual size" and ignores the actual value. Never set a Form child's width or height to exactly 1.

3. **Chain constraints anchor to the Form's edges, not siblings.** `IswNleft, IswChainRight` means "keep my left edge at a fixed offset from the Form's right edge." It has nothing to do with any sibling widget.

4. **`fromHoriz` sets initial placement only.** Chain constraints determine how the widget moves during Form resize. If `fromHoriz` and chain constraints conflict, the result is unpredictable.

5. **`IswResizeWidget`/`IswConfigureWidget` bypass the Form.** They set core geometry directly without triggering a Form relayout. Sibling widgets will not move. Use `IswSetValues` on the child's width/height to go through the Form's GeometryManager (requires `resizable=True`).

6. **Form cannot do "fill remaining space."** There is no Form equivalent of `flexGrow`. If you need one child to absorb leftover space, use FlexBox instead.

## Box

`#include <ISW/Box.h>` -- `boxWidgetClass`

### Key Resources

| Resource | Purpose |
|----------|---------|
| `orientation` | `XtorientHorizontal` or `XtorientVertical` |
| `hSpace` / `vSpace` | Horizontal/vertical spacing between children |

### Notes

- Box gives every child its requested size. No stretching, no flexible distribution.
- Box is suitable as a child container inside FlexBox or Form when you need simple stacking (e.g. a vertical stack of labels for a clock, or a horizontal row of tray icons).
- Box does not have constraint resources -- child positioning is purely sequential.