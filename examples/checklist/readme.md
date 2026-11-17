# Checklist add-on

A persistent checklist with Add, completion checkboxes and Remove. General
settings can place new items at the top. It uses API 1 and shared Pilon controls,
with no runtime, network activity or external dependencies.

From the repository root:

```sh
python3 src/scripts/compile-addon.py examples/checklist --output build/checklist.piext
```

Install `build/checklist.piext` through Settings → Add-ons, then enable Checklist.
The host supplies the 16px page margin; the page does not add another one.

`addon.setting`/`addon.setSetting` store `items` (an array of `{title, done}`)
and `addAtTop` (a boolean) under `org.example.checklist`. Writes happen before
the visible list changes. A failed write retains saved items and the input draft.
Malformed saved items are not overwritten. A nonempty unsubmitted item blocks
disable through `prepareDisable()` until it is added or cleared.

The settings section works while the page is unloaded. It does not
create the page or start work; each Add reads the current insertion preference.
Disabling the add-on preserves its settings and list. Packages are trusted
in-process QML/JavaScript, not sandboxed extensions.

The source tests are in `src/tests/qml/tst_samples.qml`; see
[testing](../../docs/addons/TESTING.md) and the
[API contract](../../docs/addons/API.md).
