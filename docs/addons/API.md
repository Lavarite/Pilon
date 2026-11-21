# Add-on API 1

The executable contracts are [the compiler](../../src/scripts/compile-addon.py),
[AddonRegistry](../../src/core/addons/addonregistry.h), [AddonHost](../../src/core/addons/ui/AddonHost.qml),
[AddonSettingsContent](../../src/core/addons/ui/AddonSettingsContent.qml), and the context factory
in [Main.qml](../../src/app/ui/Main.qml). Integration and storage-owner protocols are in
[INTEGRATIONS](INTEGRATIONS.md); Home Screen publication is in
[WIDGETS](WIDGETS.md).

## Manifest

```json
{
  "id": "org.example.checklist",
  "name": "Checklist",
  "description": "Add, complete and remove saved checklist items.",
  "version": "1.0.0",
  "apiVersion": 1,
  "page": "Page.qml",
  "settings": [{"id": "general", "name": "General", "entry": "Settings.qml"}]
}
```

| Field | Contract |
| --- | --- |
| `id` | Stable lowercase namespaced ID, at most 128 characters. Dot-separated segments start with a letter and contain letters, digits or hyphens. Reserved shipped IDs: `files`, `ssh`, `assistant`, `notes`, `calendar`, `git`, `devices`. |
| `name` | Nonblank plain text, at most 128 characters, without control characters. |
| `description` | Optional nonblank plain text, at most 200 characters. Include it in new packages; older API 1 packages may omit it. |
| `version` | Nonblank release string, at most 64 characters. Use stable numeric `X.Y.Z` for new releases; only that form can satisfy versioned dependencies/integrations. |
| `apiVersion` | Integer `1`. |
| `page` | Optional package-local QML page with an Item root. Only this entry adds sidebar navigation. |
| `settings` | Optional array of at most 32 sections, each with exactly `id`, `name`, `entry`. Section IDs are unique lowercase letter/digit/hyphen identifiers, start with a letter and are at most 64 characters. |
| `runtime` | Optional package-local QML object for page-independent work. |
| `startOnEnable` | Optional boolean; true requires `runtime`. Starts it with the enabled workspace without creating the page. |
| `widgets` | Optional array of at most 16 records with exactly `id` and `name`; requires `runtime`. Widget IDs use the section-ID syntax and are unique in the package; names are nonblank, control-free text of at most 128 Unicode code points. See [WIDGETS](WIDGETS.md). |
| `lifecycle` | Optional passive cleanup QML object; see below. |
| `icon` | Optional package-local asset. No icon is required for a functional package. |
| `dependencies` | Required add-ons: records with `id`, `minVersion`, optional `maxVersion`. |
| `extensionPoints` | Optional custom contribution targets: records with `id`, `apiVersion`. Core supplies `page` and `settings` API 1 without declarations. |
| `integrations` | Optional contributions: records with `id`, `name`, `target`, `minVersion`, `point`, `apiVersion`, `entry`, and optional boolean `enabledByDefault` (false when omitted). |

At least one of page, runtime, nonempty settings/integrations or lifecycle must
exist. Unknown fields are rejected. The manifest is at most 64 KiB. Arrays of
dependencies, extension points and integrations have at most 32 records.
Dependencies cannot repeat IDs or refer to self. Point/integration IDs use the
section-ID syntax and are unique within their respective arrays.

Entry paths are case-sensitive, relative to the package root. No absolute paths,
URLs, backslashes, control characters, empty segments, `.` or `..` are accepted.
Entries must exist inside the source/bundle. Duplicate package IDs are errors;
directory order is not an override mechanism. Compiler and registry validate the
same schema.

### Versions and dependencies

Version comparison accepts exactly three nonnegative numeric components, without
leading zeroes, each at most 2147483647. Minimum is inclusive; an optional maximum
is exclusive. No prereleases, build labels, caret/tilde ranges, alternatives,
version downloads or dependency solver are implemented. A legacy nonnumeric
manifest version can load, but cannot be a compatible dependency target.

```json
"dependencies": [
  {"id": "org.example.storage", "minVersion": "1.0.0", "maxVersion": "2.0.0"}
]
```

The registry validates the entire installed dependency closure, rejecting
missing/incompatible packages and cycles before enablement. It saves the required
enable set together and rolls back failed preference writes. Install missing
packages before enabling their dependents. When a user disables a required
provider, Pilon lists the enabled direct and transitive dependents for
confirmation and disables the whole group together. Disable the group before
removing the provider. Optional integrations remain separate from this closure.

## Entry context

Every entry declares `required property var addon`. Pages/settings are Items;
runtime/lifecycle objects may be nonvisual. Shared controls use `import Pilon`.

| Member | Behavior |
| --- | --- |
| `addonId` | This package's stable ID. |
| `setting(key, fallback)` / `setSetting(key, value)` | Per-ID persistence; writes return a boolean. A missing read does not save the fallback. Check writes before changing visible state. |
| `canWriteSettings()` | Point-in-time scoped-settings preflight returning true or an error string. Leaves existing settings contents unchanged; still handle a later `setSetting` failure. |
| `dataDirectory` | Own managed directory; an empty result means failure. Access may create the directory. |
| `configurationOnly` / `cleanupOnly` | Settings and cleanup contexts are passive. Cleanup is additionally marked `cleanupOnly`. |
| `runtime` | Own loaded runtime in a live context; null in configuration/cleanup contexts. |
| `runtimeExports()` | Returns the already loaded own runtime's `exports`, or null. Does not activate it. |
| `activateRuntime()` | Explicitly requests the enabled runtime; returns a boolean and is gated by workspace activity. Do not call during passive initialization. |
| `dependency(id)` | Promise for a declared required provider's runtime exports (null if none); rejects undeclared, incompatible/unavailable or configuration-only requests. |
| `ownerWindow` / `uiScale` | Owning application/Settings window and current scale. Use the owner for task windows; the page is already scaled. |
| `device` | Read-only, reactive platform, owning-window geometry and fold facts; see below. |
| `widgets` | Scoped Home Screen publication, placement and activation service; see [WIDGETS](WIDGETS.md). |
| `workspaceActive` | False during setup; background feature activity must remain paused. |
| `quiesced` | Temporary owner pause during cleanup/reset, after preparation succeeds. Stop background timers/activity while true. |
| `workspaceRoot` / `settingsFile` / `settingsLocation` | Selected root, central settings path and typed local-file URL used by built-in shared controls. External add-ons use scoped `setting`/`setSetting`, not the central file. |
| `modelsDirectory` / `artifactsDirectory` | Default workspace locations; feature managers may use environment overrides outside the root. |
| `workspaceRelative(path)` | Lexical root-relative helper, or empty outside the root. This is not path/symlink validation. Native cleanup validates ownership separately. |
| `isEnabled(id)` / `selectPage(id)` | Inspect effective activation or request an enabled page. Selection never enables a disabled add-on. |
| `openOwnSettings(sectionId)` | Opens an enabled add-on's section; first section when omitted. Does not create its page/runtime. |
| `contributions(point)` | Records targeting this add-on and point. Consume enabled compatible records through `AddonIntegrationHost`. |
| `integrations()` | Optional integration choices contributed by this add-on, for its own settings. |

Optional integrations can augment another add-on's loaded page or settings
without changing its source or reserving layout space. Use the built-in `page`
or `settings` point with API 1; the entry receives that loaded root Item as
`target`. Settings contributions stay configuration-only. Custom points remain
available when a target provides a more specific interface. See
[UI augmentation](INTEGRATIONS.md#ui-augmentation).

Integration/context/guard methods are detailed in [INTEGRATIONS](INTEGRATIONS.md).
Built-in native manager references and feature-specific navigation bridges in
Main are implementation APIs, not a promise that every external package can
access every built-in feature. API 1 has no native-plugin loader, permission
sandbox or external dependency installer.

Settings reads are method calls, not reactive signals. Read when acting or own
and synchronize local state. Native list values may be QML sequences rather
than JavaScript arrays; normalize JSON-compatible data before `Array.isArray`
validation and test a real registry round trip.

Keys are 1–128 letters/digits/dots/underscores/hyphens, starting with a letter or
digit. Values are JSON-compatible null, booleans, strings, finite numbers, lists
and maps; native objects, nonfinite numbers and excessive nesting are rejected.
Preferences are plain INI data, not a secret store or document database.

### Device-specific features and layouts

`addon.device` exposes facts to the existing QML entry; it does not load a second
copy of the add-on or grant a native-plugin capability. Use documented services
and check their availability before offering platform-specific actions.

| Member | Meaning |
| --- | --- |
| `platform` / `mobile` | `Qt.platform.os`; mobile is true for Android. |
| `uiScale` | Pilon's current interface scale, also available as `addon.uiScale`. |
| `windowSize` | Owning window's width and height in Qt logical pixels. Settings entries observe the Settings window. |
| `safeGeometry` | Owner-relative rectangle excluding system safe-area insets. |
| `usableGeometry` | Owner-relative usable area. The main window selects a pane around a separating or occluding fold; a secondary window supplies its own usable rectangle. |
| `workspaceGeometry` | Owner-relative content area chosen by the host. The main window avoids an occluding fold and otherwise uses its safe area; a secondary window falls back to its usable area when needed. |
| `foldBounds` | Fold rectangle in owning-window coordinates; an empty rectangle means no reported fold. |
| `foldPosture` | Platform posture string, empty when unavailable. |
| `foldVertical` / `foldSeparating` / `foldOccluding` | Reported fold orientation, separation and occlusion facts. |

These values update on resize, scale and fold changes. Bind a page's layout to
its actual allocated `width`/`height`; geometry is expressed before the host's
`uiScale` transform, so do not apply that scale a second time. Preserve models,
selection and drafts when reflowing. A platform name or a wide physical display
does not establish how much space the entry currently has. Unsupported fold
reporting gives neutral values, not an inferred device model or posture.

Home Screen widgets use their native allocation instead of this Activity
or application-window geometry. They render cached data without running QML.
The shared [widget snapshot](WIDGETS.md#snapshot-version-1) optionally supplies
a structured heading, row group, leading text and row accent for native agenda
layouts. Optional `timeGrid`/`timeRange` minute intervals select positioned timed
boxes, with a separate `allDay` strip. A frame's optional declared `actionId`
selects its header route; `open` remains the default. Optional `clock` supplies
an exact Gregorian date and explicit time-zone identifier for the current-time
line; native readers hide the line when they cannot resolve that zone. Android
uses a native scrolling time grid. Clock updates follow OS scheduling budgets,
not a live per-minute guarantee.
See [the clock and native contracts](WIDGETS.md#snapshot-version-1).
These fields are available to every provider; omitting `timeGrid` preserves the
list form. They do not add
native extension classes or executable action types.

## Preparation and cleanup

A page, runtime, integration entry or loaded settings section may implement
`prepareDisable()`: return true, a useful nonempty reason, or a Promise resolving
to either. Save owned work before approval. Rejection/exception vetoes the
operation. The host includes retained hidden settings and affected integrations
before disable, close/quit, removal or reset. It does not impose a generic timeout;
bound owned asynchronous work and release timers/callbacks on destruction.
Navigation retains page state; disabling unloads entries and removes settings
navigation only after preparation and persistence succeed.

For cleanup/reset, Core first runs preparation hooks, then sets `addon.quiesced`
and checks loaded entries on the next event-loop turn. An entry may provide
`isQuiesced()`, a synchronous boolean acknowledgement that its activity has
stopped; it must not return a Promise. Built-in native owner runtimes acknowledge
their manager's inactive state. A false acknowledgement stops cleanup before
authentication changes. Failed cleanup releases the pause; successful reset
keeps owners paused until destruction. This pause is separate from ordinary Git
preparation, whose native begin/reload transaction owns the storage lease.

A declared lifecycle entry provides all three methods:

```qml
function prepareCleanup() { return true }
function signOutAll() { return true }
function ownedPaths() {
    return {personal: [], downloads: [], addonData: ["addon-data/" + addon.addonId]}
}
```

The first two return true/reason or a Promise. `ownedPaths()` returns the map
(or a Promise), with only `personal`, `downloads`, `addonData` arrays of
workspace-relative paths. It must not claim absolute/outside/symlink paths or
another owner's data. Omitted groups are empty. Do not create files merely to
report ownership.

Cleanup loads passively on explicit removal/reset, including for disabled owners.
All ownership and preparation checks precede sign-out; reset's native path
preflight also precedes sign-out. A failed owner prevents completion, preserves
retryable account metadata, and identifies the owner. Missing declared cleanup
code must be restored; it is not treated as successful logout. Core remains
feature-neutral and never guesses how an add-on authenticates.

A lifecycle may also implement `resetPreferences(apply)`. Reset calls it with
`false` during preflight, before any account sign-out; check that the intended
preference changes can be saved without mutating them. After every sign-out
succeeds it calls the hook with `true`, before completing reset. Missing hooks
return success. Return true/reason or a Promise as above; both phases must be
idempotent. Reset only preference keys owned by the feature. Never erase an
entire scoped INI file when it also contains personal state: Counter's count
and Checklist's items share storage with their preferences. Removal does not
invoke this hook.

For scoped preferences, use `addon.canWriteSettings()` in the false phase. It
checks the registered owner's path safety and writability and opens/cancels an
atomic save without changing settings contents or leaving a new settings file.
It is not a lock: permissions, disk space or the filesystem can change before
the apply phase, so every actual write must still be checked.

Reset uses three keep groups, all checked by default: personal files, downloaded
files and add-on data. External files are always retained. A successful removal
records a tombstone and retained ownership, so restart does not restore a removed
shipped package and a later reset still knows its retained data. Disable is not
removal and does not sign out accounts.

## Storage, discovery and trust

The selected root contains `addons/`, `addon-data/<id>/`, `Models/`, `Artifacts/`
and private `.pilon/` state, including central settings and registry receipts.
Scoped preferences live in `addon-data/<id>/settings.ini`. Installation validates
a staged package before replacement. Files dropped into `addons/` are discovered
automatically; repaired/renamed files cause watches to be rearmed. A loaded valid
snapshot remains active when its source changes/disappears and requires restart
before new code replaces it. Do not edit resource/cache copies as source.

The normal OS configuration location retains `workspace.ini`, the selected-root
pointer. Initialization stages migration, preserves personal source files, refuses
different destination contents and publishes the pointer last. An initialized
missing root is an error, not permission to silently create a fresh profile.

QML/JavaScript executes in-process as the user. Namespaces avoid accidental
collisions, not hostile access. Binary resources are extractable; never bundle
credentials. Available native types can access files/accounts, and a loop or
native crash can block/crash Pilon. See Qt's [shared security model](https://doc.qt.io/qt-6/shared-security-model.html).
