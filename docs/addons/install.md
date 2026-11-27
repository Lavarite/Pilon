# Manage add-ons

## Install and enable

1. Open **Settings → Add-ons** and select the **+** card.
2. Choose one or more trusted `.piext` packages.
3. Enable the add-on. Pilon checks that its required dependencies are installed.

You can also copy a package into the selected workspace's `addons/` folder.
Pilon discovers it and moves it into `addons/<id>/<id>.piext`.

New external packages start disabled. Initial built-ins are Files, SSH,
Assistant, Notes and Calendar. Enable Devices separately. Pilon runs add-ons
as your user, so install packages from sources you trust.

## Tabs and settings

- Choose **Reorder sidebar tabs**. Drag a tab or use **Alt+Up** and **Alt+Down**.
- Clear a tab's checkbox to hide its page while keeping the add-on active.
- Enabled add-on settings appear in the main Settings menu.
- On narrow screens, swipe the bottom bar or use Left and Right to reach more tabs.

## Disable, update or remove

Turning an add-on off keeps its folders, files, preferences and accounts.
Built-in and core Settings remain available when all add-on pages are off.

- Restart Pilon after replacing a package from outside the app.
- To remove a package from Pilon, disable it first, then use **Remove** on its card.
  If account cleanup fails, resolve the reported error and retry.
- To restore a removed built-in, import its `.piext` from the Linux bundle.

## Recover from a startup failure

Quit Pilon, then start it with add-ons temporarily paused.

```sh
pilon --disable-addons
```

Recovery mode keeps your saved activation choices.

## Build an add-on

Follow the [authoring guide](index.md) or start with
[Counter](../../examples/counter/readme.md) or [Checklist](../../examples/checklist/readme.md).
