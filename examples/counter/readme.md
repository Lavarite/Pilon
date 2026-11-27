# Counter add-on

Counter demonstrates an API 1 page, passive settings section and lifecycle
entry. The page persists `count` before updating its label. Settings store the
increment as `step`; the next click reads the current value. Lifecycle preference
reset restores `step` while preserving the count.

From the repository root:

```sh
python3 src/scripts/compile-addon.py examples/counter --output builds/examples/counter.piext
```

Import `builds/examples/counter.piext` in **Settings → Add-ons**, then enable Counter.
Use your own namespaced manifest ID when copying the example. The host supplies
the page margin, and shared controls come from `import Pilon`.

The package has no runtime, network activity or external dependencies. It uses
scoped plain settings, not a credential store. `.piext` packages run as trusted
code in Pilon's process.

See the [authoring guide](../../docs/addons/index.md),
[API](../../docs/addons/API.md) and [tests](../../docs/addons/TESTING.md).
Counter is intentionally small; [Checklist](../checklist/readme.md) shows
retained drafts, validated list data and visible write-error handling.
