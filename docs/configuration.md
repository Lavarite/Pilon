# Workspace configuration

Open **Settings → Application → Configuration** to export or import a `.piconfpack`.
The file picker lets you choose its destination. Import shows the changed settings and missing add-ons before Pilon saves open work and reopens the workspace.

## Edit settings

The selected Pilon folder contains `config/pilon.piconf`. Pilon creates it from your existing preferences. It holds appearance, application settings, shortcuts, sidebar layout and add-on declarations. Each add-on has its own `addon-data/<id>/config.piconf` for its preferences and integrations. Changes made in Settings update these files. Look for the gear icon in Files to edit a configuration. Restart Pilon after editing it in a text editor.

For example, put these entries in `config/pilon.piconf`. Add to the existing sections when they already exist.

```toml
format = 1
include = ["theme.piconf"]

[appearance]
accent = "${vars.accent}"
frostiness = 66

[addons."org.example.timer"]
enabled = false
source = "https://github.com/example/timer"
config = "../addon-data/org.example.timer/config.piconf"
```

In `config/theme.piconf`, define the shared colour.

```toml
[vars]
accent = "#e79d61"
```

Use quoted strings, numbers, booleans, JSON arrays and objects. Lines may contain `#` comments. Paths in `include` and `config` are relative to the file declaring them. An entire quoted reference such as `"${vars.count}"` keeps the variable’s type. In a longer string, it inserts text. Write `$${vars.name}` for a literal reference. Duplicate keys and reference cycles report the file and line.

## Transfer a workspace configuration

A bundle contains the original UTF-8 text of the main file, its includes and the referenced add-on configurations, including disabled and missing add-ons. Credentials remain in the local secret stores. Documents, chats and executable packages stay on their originating device. Check external folder paths on the receiving device. Keep secrets out of files you include by hand.

Declare a package source as `builtin`, `local`, or an HTTPS URL. Optional `version` and `sha256` fields pin a particular package. An offline import preserves a missing package’s declaration. Use its source in Application settings to obtain the package and approve installation.

If Pilon reports a conflicting edit, review the current files before applying the import again. If a hand-edited file prevents startup configuration from loading, Pilon keeps its last working settings. Correct the file and restart, or import a known-good bundle. The restore prompt lists the files it will replace.
