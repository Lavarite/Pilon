# Workspace settings

## Export or import settings

1. Open **Settings → Application → Configuration**.
2. Export a `.piconfpack` or choose one to import.
3. Review changed settings, missing add-ons and files to restore before applying.

A bundle contains settings, included files and add-on declarations. Credentials
stay in the device's credential store. Documents and chats stay in the workspace.
Review device-specific paths after transferring a bundle.

Use the bundle when moving settings between devices. If Android shared storage
rejects a workspace move, Pilon keeps using the current workspace.

## Edit settings in a text editor

| Settings | Workspace path |
| --- | --- |
| App and add-on declarations | `config/pilon.piconf` |
| An add-on's preferences | `addons/<id>/data/config.piconf` |

Restart Pilon after editing. Paths in `include` and `config` are relative to
the file that declares them. `addon-data/<id>/` in older files and bundles points
to the same add-on data folder.

<details>
<summary>Example with an included file</summary>

In `config/pilon.piconf`, add to the existing sections:

```toml
format = 1
include = ["theme.piconf"]

[appearance]
accent = "${vars.accent}"
```

In `config/theme.piconf`:

```toml
[vars]
accent = "#e79d61"
```

Values support strings, numbers, booleans, JSON arrays and objects. A whole
reference preserves the value's type. Write `$${vars.name}` for a literal
reference. Pilon reports duplicate keys and reference cycles with a file and line.

</details>

## Restore an add-on

An imported bundle can declare a `builtin`, `local` or HTTPS source, with
optional `version` and `sha256` pins. Use the declaration in **Application**
settings to approve a missing package's installation. Its settings are
available when the package is installed, including after an offline import.

If a file conflict blocks import, review the current files first. If a direct
edit prevents loading, correct the file or import a working bundle.
