# Home Screen widget contract (API 1 addition)

Widget metadata is `widgets: [{"id":"agenda","name":"Agenda"}]` in an add-on
with a runtime. IDs are unique section IDs, with at most 16 entries. Names are
nonblank, control-free text of at most 128 Unicode code points. A widget provider
needs no page or sidebar item. Existing API 1 packages need no widget declaration.
Calendar is the initial shipped provider.

## Scoped service and placement

The runtime uses `addon.widgets`; ownership always comes from its add-on context.
The service accepts only widget IDs declared by that enabled provider.

| Member | Contract |
| --- | --- |
| `available` | Native widget support is available. False on platforms without the adapter. |
| `canRequestAdd` | The launcher supports an in-app pin request. False does not mean the OS widget gallery is unavailable. |
| `error` | Actionable explanation of the most recent failed operation. |
| `publish(id, snapshot)` | Atomically saves validated provider data and requests a native reload; returns a boolean. Failure preserves the previous snapshot. |
| `clear(id)` | Removes this provider's derived snapshot and reloads the native display. Its declaration and OS configurations remain. |
| `generation(id)` | Last valid stored generation, or zero when missing/invalid; use a larger positive generation for the next publication. |
| `requestAdd(id)` | Explicit user action to activate the provider runtime and request supported OS placement; returns whether the request started, not whether the user accepted it. |
| `refreshRequested()` | Signal asking a loaded runtime to publish current data, including after application resume. |
| `activated(widgetId, actionId, data)` | Signal delivering a validated action and current data only to its owning runtime. |

Settings construction stays passive. A configuration-only context cannot publish
or clear snapshots; an explicit placement action can activate the live runtime.
Setup, disabled/quiesced owners, cleanup and in-progress lifecycle changes block
publication/placement. Native callbacks never instantiate QML.

Settings → Add-ons exposes the metadata-driven **Add widget** action for an
enabled provider on a supported platform. A single declaration requests that
widget directly; multiple declarations use the shared action menu. Android uses
supported launcher pinning, or reports that the user should use the OS widget
gallery. Native configuration selects a currently declared provider from the
catalogue.

## Files and ownership

The private platform directory is `files/pilon-widgets` on Android.
`catalog.json` has:

```json
{"version":1,"workspaceId":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","providers":[
  {"addonId":"calendar","widgetId":"agenda","name":"Agenda"}
]}
```

A provider file is the lower-case SHA-256 of UTF-8 `addonId/widgetId`, followed
by `.json`. Native readers derive this filename; catalogues never supply paths.
Publication is atomic. All readers validate shape, IDs, bounds and workspace
identity and reject linked/nonregular cache files. The catalogue has exactly
`version`, `workspaceId`, and `providers`, at most 1,024 providers, at most 16 per
add-on and at most 256 KiB. Each provider has exactly `addonId`, `widgetId`, and
`name`; `addonId/widgetId` pairs are unique. Provider files are derived data,
never credentials or databases. Calendar event text is included only as needed
for the displayed agenda; no account material is exported.

## Snapshot version 1

Provider input has exactly `version`, `generation`, `updatedAt`, `validFrom`,
`validThrough`, `palette`, `timeline`, and `actions`. The store adds `workspaceId`,
`addonId`, and `widgetId`. Workspace IDs are lower-case SHA-256 hex values of
length 64 derived by the store from the canonical selected workspace root.
Date-times use UTC ISO 8601 seconds with `Z` and optional 1–3 fractional digits;
invalid calendar dates, offsets and higher precision are rejected. Generations
are integers 1 through 9007199254740991. Palette has exactly `background`, `text`,
and `accent` as `#RRGGBB` or `#AARRGGBB`. Use Pilon's theme roles; native system
tint/background behavior can affect their presentation.

A frame requires `at`, `title`, and `rows`, with optional `heading`, `timeGrid`,
`actionId`, and `clock`; frame times increase, within the
validity interval, with the first equal to `validFrom`. `updatedAt` is within
`[validFrom,validThrough)`, and `validThrough > validFrom`. A row has `id`,
`title`, optional `detail`, optional `actionId` and the presentation fields below. IDs use ASCII letters,
digits, underscores or hyphens, 1–128 characters. Row IDs are unique within a
frame. Text is plain, control-free (except LF in `detail`/`leading`), with valid Unicode
surrogate pairs; title is at most 256 Unicode code points and detail 512.

The optional presentation fields are generic data, not Calendar-specific native
APIs. They do not change action routing, ownership, validity or existing limits.

| Field | Contract |
| --- | --- |
| Frame `heading` | Object with exactly `label`, `value`, and `caption`, all plain strings without control characters or newlines. Their limits are 128, 16, and 128 Unicode code points respectively. |
| Row `group` | Plain group label of at most 128 code points, without controls/newlines. Native views display a label when it differs from the preceding row's group. |
| Row `leading` | Plain leading-column text of at most 128 code points. LF is allowed; other controls are rejected. |
| Row `accent` | Exactly `#RRGGBB` or `#AARRGGBB`, used for the list's accent bar or the time-grid box colour. |
| Frame `timeGrid` | Object with exactly `startMinute` and `endMinute`, integer JSON numbers satisfying `0 <= startMinute < endMinute <= 1440`. Selects the generic one-day time-grid renderer. |
| Row `timeRange` | Independently optional interval with the same exact keys and bounds. Supplies the event's start and end within the displayed day. |
| Row `allDay` | Optional actual boolean, false when omitted. True cannot coexist with `timeRange`; these rows occupy the separate all-day strip. |
| Frame `actionId` | Optional action ID, using the existing ID rule and referring to this snapshot's `actions`. The header/widget tap uses this instead of `open` when present. |
| Frame `clock` | Object with exactly `date` and `timeZone`, both strings. `date` is a real Gregorian date in exact `YYYY-MM-DD` form, years 0001–9999. `timeZone` is 1–128 ASCII characters matching `[A-Za-z0-9_+-]+(?:/[A-Za-z0-9_+-]+)*`: nonempty path segments, without dots, colons, whitespace or empty segments. |

Each field is optional independently; a present field must have the documented
type, so null is rejected. Minute intervals reject booleans, strings, fractions,
unknown keys and invalid/reversed ranges. Required frame `title` remains the
text fallback. Omitting `timeGrid` retains the existing list presentation,
including optional headings, groups and leading text. Other add-ons can use
either form without a Calendar dependency.

In a time grid, interval minutes describe one publisher-selected day; native
views do not infer dates or zones from labels. The compact header uses `frame.title`.
Timed boxes follow the row's position and duration within the published interval.
Android scrolls that interval in a native `ListView`, retaining a fixed header
and full-duration boxes; a short allocation shows fewer hours at once.
Very short events use the Calendar page's font-based minimum visual
height (about 34 points/dp before text scaling) before overlap assignment;
their actual interval/time text is retained.
The hourly scale remains readable (about 60 points/dp per hour,
scaled with text); a shorter allocation shows fewer hours instead of squeezing
24 hours into the widget. The 24-hour axis and boxes use Pilon's Calendar
appearance: 5-unit corner radius, event-colour fill at 0.22 opacity, border at
0.65 opacity, and 2-unit gaps between overlap lanes. Titles/time labels remain
about 14/12 points or sp before system text scaling; short boxes omit optional
fields rather than reduce the font. Accessibility labels keep the title, time and
a description preview. Open the event to read its full description.

Overlapping events use the Calendar page's greedy lane assignment and expansion
into unoccupied adjacent lanes. Android uses one or two readable lanes, requiring
about 96 scaled units per lane; its overflow count covers hidden lanes across
the scrollable interval. Events in later hours remain reachable by scrolling.
All-day rows use one bounded strip plus an overflow count, not timed boxes.
On tiny allocations the strip moves into the single status footer to protect
the timed viewport. Provider status and known hidden-event counts share that
footer. When even a footer would remove readable timed content, the header's
accessibility text retains the omitted summary. Rows with neither `timeRange`
nor `allDay: true` are status text outside the plot.

Calendar publishes one relevant day's grid: the current day when work remains,
otherwise the next day with events within seven days. It anchors the grid at
the first relevant timed hour (or the current hour for an ongoing event) and
keeps the end at minute 1440; allocation controls how much is visible. Its
frame action identifies the displayed date/anchor minute through ordinary scoped
action data, so header taps open that period. The native renderer interprets
only the action reference, not its data.

`clock` is independently optional; a list accepts it without drawing a marker.
A syntactically valid but unknown zone does not invalidate the snapshot. Native
readers hide the line when their zone database cannot resolve the identifier.
When the current Gregorian date in that explicit zone equals `clock.date` and
its minute is inside `[timeGrid.startMinute,timeGrid.endMinute)`, the grid shows
a one-dp/point line in `palette.accent`, at the existing hour pitch, without a
label or icon. Calendar publishes its selected date and known configured zone;
unsupported custom offset identifiers omit `clock` while preserving the grid.

The clock is best effort. Android uses one shared, non-wakeup elapsed-realtime
alarm with a ten-minute inexact delivery window for active clock grids; sleeping
or power-restricted devices can update later. It does not use the exact reminder
alarm, a foreground service or a new permission. The adapter does not promise
continuous or exact per-minute movement.

This is an additive version 1 schema extension in the updated readers. Older
snapshots remain valid; reading snapshots that include the new fields requires
the matching updated native adapter build.

`actions` is an object mapping up to 512 action IDs to flat objects with at most
8 named scalar values. Keys use the same ID rule. Values are plain strings
(up to 1,024 code points), booleans, finite numbers, or null. Payloads are delivered
only to the publishing runtime, never interpreted as files, URLs or commands by
core. An `open` action remains required as the default header/app route. A row's
optional `actionId` must exist in that snapshot's `actions` map. Nested values
and unknown schema keys are rejected.

Limits: 256 KiB serialized provider file, 64 frames, 64 rows per frame and 512 rows
total. A missing/expired/corrupt cache is unavailable, never an empty agenda.
The current frame is the last `at <= now` inside `[validFrom,validThrough)`.

## Native actions and refresh

Widget taps open `pilon://widget/<addonId>/<widgetId>` with `workspace`, `action`,
and `generation` query values, selecting frame `actionId` or the default `open`
for the header. Those are the only query fields: duplicates,
extra paths, user info, ports, fragments and malformed identities are rejected.
Android header/direct pending intents are explicit and immutable. Collection
children require a mutable pending-intent template and fill-in data. That
template fixes Pilon's Activity component, collection action and widget ID;
the supplied URI must still match the owned instance's current workspace,
provider declaration and binding before normal current-snapshot validation.
Fill-in fields cannot replace the base component, action or widget ID. Widget
action resolution accepts only the validated URI reference; arbitrary intent
fields do not supply action data, file paths or commands to that resolver.
Pilon checks current workspace, enabled declaration, snapshot validity and action
existence. It resolves action data from the current snapshot; an older generation
can only open an action still present there. Future generations are rejected.
Nothing in an incoming URL supplies action data or executable code.

Providers republish after relevant domain mutations, sync and palette changes,
and handle `refreshRequested` after resume. Core activates providers with active
native configurations when the application is running, using the normal
runtime loader rather than a separate widget runtime.
Native callbacks read the bounded cache without starting Qt or a foreground
service. Platform reload budgets apply. Providers publish future frames and
truthful coverage; UI requests a refresh when coverage expires. Disable/removal/
reset/workspace change removes the provider from the catalogue and requests a
neutral OS reload; OS-cached pixels cannot be promised instantaneous erasure.

Android uses one AppWidgetProvider and RemoteViews, configured per native widget
instance to select a provider. Its time grid uses one continuous collection item
with a stable ID to preserve scroll through same-period updates. Android 12 and
later receive the complete item with the widget update. The launcher can restore
it without binding to Pilon. The widget uses its reported allocations to provide
one- and two-lane timelines within the Binder size limit. Older Android versions
use a private service protected by `BIND_REMOTEVIEWS` that reads the bounded
cache and returns neutral content for invalid or revoked bindings. Neither path
starts Qt.
Each declared widget has one shared publication; multiple native copies may
select it and independently resize. Widget allocation, rather than
`addon.device.windowSize` or fold bounds, controls its native layout.

Follow the
[widget test matrix](TESTING.md#device-layouts-and-home-screen-widgets) and report
native checks separately from schema/source validation.
