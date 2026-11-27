# Add-on interface rules

Preserve Pilon's existing design and the exact task scope. Read the repository
[AGENTS.md](../../AGENTS.md) and persistent SOUL rules before interface work. Do not
add slogans, generic introductions, decorative descriptions, sample items,
colours, icons, shapes, animations or shadows unless explicitly requested or
necessary to operate the feature. Manifest descriptions explain function; they
do not justify repeating an introduction inside the page.

## Controls and layout

Use `import Pilon` and shared components from [src/ui](../../src/ui):

| Need | Shared control |
| --- | --- |
| Action | `PilonButton`, `PilonToolButton`, `PilonPanelButton` as appropriate to existing examples |
| Action with a long label | `PilonWrapButton` |
| Existing hold-to-confirm action | `PilonHoldButton` |
| Text input | `PilonTextField`, `PilonTextArea` |
| Boolean choice | `RoundedCheckBox` |
| Selection/number | `PilonComboBox`, `PilonSpinBox`, `PilonSlider` |
| Labeled settings field | `PilonFormRow` |
| Meaningful settings group | `PilonSectionHeader` |
| Scrollable form/list | `PilonScrollView` |
| Native list, grid or document scrolling | `PilonScrollMotion` on the viewport and `PilonScrollBar` for overflow |
| Context/action menu | `PilonMenu`, `PilonMenuItem` |
| Short confirmation | `PilonConfirmation` with a message and a specific action |
| Operation failure | `PilonErrorPopup` owned by the initiating page or form |
| Existing task popup | `PilonPopup` with the appropriate owning window |

Use inherited palette and font rather than hardcoded colours/type sizes. Give
layout children `Layout.minimumWidth: 0` where long implicit widths could push
controls outside the viewport. Wrap long labels and error text; reflow actions
into one column or use scrolling instead of truncating required controls.

When retaining separate versions of native map/list data, snapshot the
JSON-compatible value before caching it.
QML may retain a live wrapper around a native property; a shallow copy alone can
make an older preview show newer data. Test the provider reusing its object.
When multiple visible views share a manager's last-preview property, own each
view's cached result and pending request. Accept only matching root/path/operation
results; another view's read must not invalidate the cache or start a read loop.
Invalidate on actual file mutations and test two visible views.

The external page host already applies 16px margins. A page should fill its
given area without another outer 16px inset. Use 8px gaps between controls and
12px between panels/groups. Settings entries fill the supplied settings page
area; avoid a redundant page title or outer card.

`PilonFormRow` places its label beside the first field on wide screens and above
the field group with an 8px gap on narrow screens. `PilonSectionHeader` provides
the established label/rule for a real group; do not add groups to a single field
merely for decoration.

## Interaction

Every action must be reachable by Tab and keyboard activation. Give controls
clear visible labels or accessible names; contextual Remove actions can include
the item's name in `Accessible.name`. Preserve focus when updating rows and move
it to an adjacent action/input when removing the focused item. `PilonScrollView`
reveals focused controls, including controls moved by layout changes.

Context/action menus stay inside the application at the cursor/button; combo
dropdowns open at their control. Do not give menus a title bar, close button or
reserved title-bar space. Use `PilonMenuItem.shown` for conditional actions so
hidden actions do not leave gaps.

Task forms/dialogs use a separate owned window with one close button inside the
panel's top right and no minimise/maximise buttons or added title heading. Follow
existing shared window/popup ownership rather than creating parentless modal
windows. Closing a parent must not leave an invisible modal blocker. Main
Settings is the host for manifest sections; do not build another settings shell.
Disabled add-ons have no settings sections. An enabled add-on's settings remain
passive: do not start its runtime/page from a constructor. Retain drafts across
section changes and include hidden loaded forms in preparation. Use the shared
label/rule header for distinct tasks, without headings around a single control.
Background entries follow [the quiescence acknowledgement contract](API.md#preparation-and-cleanup).
Forms use `PilonScrollView`. Native lists and grids keep their own viewport and
use `PilonScrollMotion { view: viewport }` for the same wheel, touch and edge
behavior. A fresh press stops momentum and reaches the control on the same click
or tap. Scrollbars appear when content overflows.

Keep everyday actions visible and put secondary actions in the existing menus
or a focused task form. Icon-only actions need a tooltip and accessible name.
Do not replace actionable status or errors with an unexplained icon. SSH keeps
the editable username beside `@` and a read-only device address. Reflow the full
group at narrow widths so every field stays available.

Show an operation failure over its owning view. Bind `message` to the error, clear it on dismissal or a new attempt, and use `details` or a recovery action when needed. Opening or dismissing the popup must preserve focus, selection, scroll position and layout. Empty-state
copy should identify the state only when needed, such as Checklist's No items.
Do not use decorative bullet characters or middle-dot separators.

## Required checks

Exercise normal and narrow layouts, application scale 1 and 1.4, long text and
larger application fonts. Check keyboard focus/scrolling, actual pointer clicks,
failed saves and open/close/reopen. A correct screenshot does not prove that a
transparent overlay or disabled ancestor allows input. See [TESTING.md](TESTING.md).
