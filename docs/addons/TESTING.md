# Verify an add-on

Use isolated test data. Never point a test at the user's add-on directory,
notes, Calendar database, account credentials or Downloads files. The native
Qt Quick test runner creates temporary configuration/data roots. CTest also
supplies a deny-by-default `secret-tool` shim so ordinary suites cannot access
the real keyring.

## Source behavior loop

Write the behavior/failure test first, run it to demonstrate the missing
behavior, implement the smallest change, then run the focused suite. For this
repository:

```sh
cmake -S src -B build -DPILON_BUILD_TESTS=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure -R '^(addons|addons_ui|samples_ui)$'
```

`addons` verifies real registry/package/storage behavior. `addons_ui` verifies
host/page/settings lifetimes. `samples_ui` exercises the source samples. Run
relevant shared controls/settings suites when changing their use, and the full
CTest suite before integrating application changes. Coordinate builds with
other agents to avoid simultaneous builds of shared targets.

For a single Checklist function without rebuilding QML:

```sh
PATH="$PWD/build/test-tools:$PATH" QT_QUICK_BACKEND=software \
  PILON_NOTES_HANDWRITING=0 PILON_EPHEMERAL_CHAT=1 PILON_CODEX=/nonexistent \
  build/pilon_notes_ui_tests -platform offscreen \
  -input build/test-fixtures/src/tests/tst_samples.qml Samples::test_addCompleteRemoveAndRecreate
```

The supported mock is a scoped object passed through the required `addon`
property, implementing only the context members that the sample uses. Its
`setting` returns a fallback for missing values; `setSetting` returns false for
injected write failures and copies JSON-compatible saved data. Do not fake global
Main.qml IDs or implement a second production registry inside a test.

Test visible outcomes: items/actions, persisted values after recreating entries,
blocked drafts and unchanged data on failure. Native registry tests separately
verify per-ID isolation and disk write behavior. A source mock test does not
prove compiled package discovery or real filesystem persistence.

## Actual package loop

```sh
python3 src/scripts/compile-addon.py examples/counter --output build/counter.piext
python3 src/scripts/compile-addon.py examples/checklist --output build/checklist.piext
```

Exercise the compiled bundles through the real registry in an isolated test
profile: install, enable, use the page, change settings, disable/re-enable and
restart. Verify that data survives, disabled settings are absent, and enabled
settings do not load the page or start runtime work. Keep old API 1 packages
without descriptions accepted. Test automatic discovery of dropped packages,
source replacement/removal requiring restart, and the `--disable-addons` recovery
path. Successful removal must remain removed after restart; explicit reimport
is distinct from reseeding a shipped package.

For registry changes, cover numeric version bounds, missing/incompatible/cyclic
dependencies, atomic preference writes, and confirmation and cancellation when
disabling a required provider and its dependents. Optional integrations use their declared default (off when
omitted), preserve saved opt-outs, require compatible enabled
endpoints and unload safely. A missing optional target must not disable its
contributor. Run `integrations_ui` for context/host lifetimes and explicit first
activation from passive settings; an independent package need not duplicate
registry parsing tests.

For cleanup, cover all-owner preflight before any sign-out, disabled/missing
owners, sequential bounded sign-out, retry metadata on failure and retained
ownership after removal. Reset uses the three generic keep groups. Exercise
resetPreferences(false) failure before sign-out and resetPreferences(true) only
after successful sign-outs; personal state in a mixed scoped store survives.
Scoped preflight must reject locked/link paths without changing existing bytes
or leaving a settings file behind; actual apply failures still need coverage.
Workspace changes also require the native `workspace` suite: source retention,
failed destination writes, bootstrap selection and roots that disappear.

The compile helper validates resources and manifest paths; it does not replace
loading the QML in Pilon. Include relative JavaScript/assets in a fixture when
the add-on uses them. Never import test files, a repository or build outputs into
the package source folder by accident.

## Interaction and layout matrix

- Add, act on and remove items with actual mouse clicks and with Tab/Space/Enter.
- Wait for the application's explicit runtime/guard readiness in ordinary Main
  fixtures; use dedicated tests for in-flight startup instead of fixed sleeps.
- Use long labels, unbroken strings, narrow widths, scale 1 and 1.4 and larger
  application fonts. Verify horizontal bounds and that focused actions scroll
  into view instead of clipping.
- Save, close, reopen, navigate away and disable. Check retained settings drafts
  and every entry's preparation hook, including hidden settings sections.
- Inject failed writes and invalid saved values. Preserve recoverable state;
  never show a failed change as saved.
- Inspect a native window and repeat ordinary pointer input. QtTest's synthetic
  offscreen events alone cannot detect every compositor/modal/input blocker.
- Confirm background work stops when disabled and settings remain passive.
- If modifying a built-in managed-file integration, run `git_sync` and the
  domain's native suite, plus `git_workspace_ui`, `git_changes_ui`, `git_adopt_ui`
  and `github_ui` for affected UI. Test incoming edits/deletions, invalid snapshots,
  unresolved conflicts, restart/Abort and failed reloads. These native contracts
  are not required for an independent Counter-style package.
- For shared preview managers, show two instances selecting different files;
  asynchronous reads stay bounded and neither view replaces the other's cached
  result. A real status/file change must refresh both views.

For Checklist specifically, cover add/complete/remove, page recreation,
`addAtTop`, write-failure rollback, invalid stored items and a nonempty input
draft veto. Counter covers a smaller page/settings example; registry isolation
must not be duplicated in each sample.

## Device layouts and Home Screen widgets

For entries using `addon.device`, cover owning-window resize, narrow/wide sizes,
scale changes, safe-area insets and reported vertical/horizontal fold bounds.
Settings must use its own window coordinates, including after moving that
window. Preserve draft text, selected items and models throughout transitions.
Headless geometry tests check reflow; a physical fold test is still needed for
native posture callbacks and system insets.

For shared widget changes, the focused CTest selection is:

```sh
ctest --test-dir build --output-on-failure \
  -R '^(addons|addons_ui|widgets|widgets_ui|widget_package_ui|calendar_widgets|calendar_widget_runtime|main_ui|controls_ui)$'
```

`widgets` verifies the real bounded store and current-action resolution;
`widgets_ui` checks owner scoping and passive/disabled/quiesced contexts.
`calendar_widgets` covers agenda generation with real Calendar data;
`calendar_widget_runtime` covers the publisher's runtime lifecycle. These are
source/native tests, not evidence that a launcher rendered a widget.
All adapters consume the same
[schema vectors](../../src/tests/fixtures/widgets/contract-v1.json); add a shared
case when changing accepted JSON instead of testing only one reader.
The 47 current cases include optional clock grammar, real Gregorian dates and
unknown-zone acceptance. A reader's source/preflight check does not establish
that its native tests executed.

`widget_package_ui` installs the compiled runtime-only
[test provider](../../src/tests/fixtures/widget-provider) through Main's real
registry. It uses the actual `WidgetStore` for catalogue writes, publication,
generation checks and action resolution; only native OS availability and pin
initiation are substituted on Linux. It checks passive Settings, explicit
activation, current action data for an older generation, and disabled-provider
revocation. This proves the compiled package/shared service route, not native
launcher placement.

Use an isolated external provider to exercise declaration, compile/import,
runtime activation and callbacks. Include a runtime/widgets-only package without
a page, multiple declared widgets, malformed metadata and an old API 1 package
without widgets. Opening Settings must remain passive; only an explicit placement
action can activate an otherwise unloaded runtime. Out-of-owner/undeclared IDs,
invalid schemas, oversized payloads and failed writes must preserve the previous
valid snapshot. Cover UTC/date precision and timezone/DST boundaries, monotonic
generations, expired/missing/corrupt caches, workspace changes, disable/removal/
reset, and old/forged action URLs. Older generations may resolve only an action
still present in the current snapshot, using its current data. A native reload
must not create a publication/reload feedback loop.

On Android, test launcher gallery configuration and supported pinning separately:
cancel/accept, actual native instance ownership, reconfiguration, multiple copies,
removal/restore, resize and narrow/wide allocations with larger system fonts.
Verify row/header taps both cold-start and resume Pilon. Kill only the isolated
app process and confirm cached rendering does not start Qt or a foreground
service. Disable a provider or change workspace and refresh: stale rows/actions
must become unavailable. A successfully initiated pin request is not confirmed
placement, and an empty agenda must remain distinct from an expired cache.

The permanent native rendering probe has a local-only builder. It needs the
existing SDK platform 37/build-tools 37.0.0, JDK 21 and standard
`~/.android/debug.keystore` (or `ANDROID_USER_HOME/debug.keystore`). It downloads
nothing and creates no signing keys. Build from any working directory; the
source/resource paths are resolved relative to the repository:

```sh
python3 src/scripts/android/build-widget-probe.py \
  --sdk "${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}" \
  --jdk "${JAVA_HOME:-$HOME/Android/jdk-21}" \
  --output build/android/pilon-widget-preview-probe.apk
adb devices -l
pilon_serial=REPLACE_WITH_CHOSEN_DEVICE_SERIAL
adb -s "$pilon_serial" install -r build/android/pilon-widget-preview-probe.apk
adb -s "$pilon_serial" shell am instrument -w \
  org.pilon.widgetpreviewprobe/org.pilon.PilonWidgetPreviewProbe
adb -s "$pilon_serial" uninstall org.pilon.widgetpreviewprobe
```

Choose the serial explicitly before the device commands. The self-targeted APK
uses its own files/cache, compiles the actual widget classes/resources and runs
the permanent `PilonWidgetPreviewProbe` assertions. It checks synthetic native
`RemoteViews` inflation, measurement, drawing and attached actions; it does not
prove real launcher placement, resizing, folding or OS tap delivery. Uninstall
the probe after testing to remove its fixtures and cached screenshots. The
builder itself never installs an APK or runs ADB.

The scroll variant builds the permanent `PilonWidgetScrollProbe` with an isolated
`AppWidgetHost`, the production provider/service and a private tap receiver. It
checks actual host scrolling, event taps, resize, cache refresh/revocation and
bounded collection payloads. On API 29+, the probe temporarily adopts only
`BIND_APPWIDGET` through `UiAutomation` for its own host setup, then drops that
identity immediately and in cleanup. No CLI or persistent bind grant is needed.
It preserves enabled accessibility services with
`FLAG_DONT_SUPPRESS_ACCESSIBILITY_SERVICES`.

```sh
python3 src/scripts/android/build-widget-probe.py --kind scroll \
  --sdk "${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}" \
  --jdk "${JAVA_HOME:-$HOME/Android/jdk-21}" \
  --output build/android/pilon-widget-scroll-probe.apk
adb devices -l
pilon_serial=REPLACE_WITH_CHOSEN_DEVICE_SERIAL
adb -s "$pilon_serial" install -r build/android/pilon-widget-scroll-probe.apk
adb -s "$pilon_serial" shell am instrument -w \
  org.pilon.widgetscrollprobe/org.pilon.PilonWidgetScrollProbe
adb -s "$pilon_serial" uninstall org.pilon.widgetscrollprobe
```

For API 28 only, a device with a compatible
[AOSP `appwidget` command](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/cmds/appwidget/src/com/android/commands/appwidget/AppWidget.java)
can grant binding to the isolated probe before instrumentation:

```sh
adb -s "$pilon_serial" shell appwidget grantbind \
  --package org.pilon.widgetscrollprobe --user current
```

Revoke that API 28 grant afterward, including after failed assertions, then
uninstall the probe:

```sh
adb -s "$pilon_serial" shell appwidget revokebind \
  --package org.pilon.widgetscrollprobe --user current
adb -s "$pilon_serial" uninstall org.pilon.widgetscrollprobe
```

Some devices block the helper VM used by this CLI. Do not disable device
execution policy or SELinux to run it. Use the automatic API 29+ test permission
where available; otherwise report a test-setup blocker. Uninstall the probe to
remove its fixture data even when assertions fail. This real test host still
does not establish acceptance in Samsung One UI or another launcher; test their
resize, gesture and tap behavior separately. A successful SDK build is not an
API-level runtime result.

## Review and delivery

Report the exact focused/full commands, outcomes and optional checks skipped.
Read the diff for accidental data access, new dependencies, decorative UI and
unsupported API claims. For a user-requested package delivery, compare delivered
bytes with the tested artifact and preserve any existing destination until an
explicit replacement is authorized. Application changes follow the repository's
build/install/launcher rule; packaging a normal external add-on does not require
editing or rebuilding Pilon's application sources.
