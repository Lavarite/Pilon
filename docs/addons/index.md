# Build an add-on

A `.piext` is a Qt binary resource containing a manifest, QML, JavaScript and
assets. It runs as trusted code inside Pilon. API 1 does not provide a sandbox,
native-plugin loader or external dependency installer.

## Examples

[Counter](../../examples/counter/readme.md) is a small page/settings/lifecycle
package. [Checklist](../../examples/checklist/readme.md) adds persisted list
state, write-failure handling and a draft that can veto disable.

Copy the example outside this checkout, choose a stable namespaced manifest ID
and numeric `X.Y.Z` version, and edit only the entries your feature needs.
Each entry declares `required property var addon`; shared controls use
`import Pilon`. An ordinary external page/settings package requires no changes
to Main.qml, CMake, the registry or native services.

## Compile and install

The compiler requires Python 3 and Qt 6 `rcc`. From the repository root:

```sh
python3 src/scripts/compile-addon.py examples/counter --output build/counter.piext
python3 src/scripts/compile-addon.py examples/checklist --output build/checklist.piext
```

Use `--rcc /path/to/rcc` if it is not found. The compiler includes every regular
file under its input folder, including hidden files; symlinks and special files
are rejected. Keep tests, repositories, native source, documentation, credentials,
downloads and compiled output outside the package folder. Shipped add-ons use
`addons/<id>/package/` as that boundary. The small examples keep only their
resources and readme together.

Import the resulting file in **Settings → Add-ons**, then enable it. See
[installation, updates and recovery](install.md). Installed packages do not need
Python unless their own feature uses it.

## Implement the contract

- Use scoped `addon.setting()` and `addon.setSetting()` for preferences. Check
  writes before updating visible state; preserve drafts and saved data on failure.
- Keep settings passive. Opening a section must not create the page or start
  downloads, sign-in or a runtime. Only explicit actions may request activation.
- Use `prepareDisable()` for owned drafts or work that must finish before an
  entry disappears. Release owned timers and callbacks on destruction.
- Declare required dependencies separately from optional integrations. Keep any
  provider export contract beside that provider and version it with the package.
- Add passive lifecycle cleanup only when the feature owns accounts or data.
  Disable retains data/accounts; explicit removal/reset runs declared cleanup.

Read the [API](API.md), [UI rules](UI.md) and [testing contract](TESTING.md) before
editing. [Integrations and ownership](INTEGRATIONS.md) defines optional UI
contributions and the separate native storage-safety protocol.
[Home Screen widgets](WIDGETS.md) use bounded snapshots from an add-on runtime;
they do not contain native code supplied by the package.

Use `addon.device` and the entry's allocated size for responsive layouts.
Android Codex sign-in token polling remains unresolved.

## Verify and deliver

Run focused behavior and failure tests in isolated data, compile the actual
`.piext`, and exercise installation, enable/disable, settings, persistence and
restart. Test mouse/keyboard input in a native window as well as automated
geometry checks. [TESTING.md](TESTING.md) lists commands and feature-specific
checks; [AGENTS.md](AGENTS.md) is the concise agent checklist.

Keep the package ID stable across releases. Deliver the tested bytes and their
source, preserving applicable license notices. Pilon's source uses GPL-3.0-only.
