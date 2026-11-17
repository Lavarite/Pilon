# Add-ons

Pilon loads trusted `.piext` packages containing QML, JavaScript and assets.
They run in the application as your user; install only code you trust. The
format is a Qt binary resource, not a sandbox or source-code obfuscation.

## Install and enable

Open **Settings → Add-ons** and select the dashed **+** card, whose tooltip and
accessible name are **Add add-ons**. Select one or more `.piext` files in Pilon's
picker. Import attempts every chosen file and reports failures. Alternatively,
copy a package into the selected Pilon folder's `addons/` directory; discovery
is automatic, with no Refresh step.

New external packages start disabled. Files, SSH, Assistant, Notes and Calendar
start enabled on new profiles; Git and Devices start off. Saved choices win.
Enabling a package validates and enables its installed required dependencies;
missing/incompatible packages must be installed first. Optional integrations
have separate saved choices and start off unless the contributor supplies an
enabled default. A package with a page adds a sidebar item;
runtime/settings-only packages do not.

Select **Reorder sidebar tabs** in Add-ons settings to open an expanded sidebar preview.
Drag a tab to change the saved order, or focus it and press **Alt+Up** or
**Alt+Down**. The sidebar updates after each drop. Clear a tab's checkbox to hide
its page without disabling the add-on; check it again to restore the page.

Enabled package settings appear as grouped sections in the main Settings menu.
Opening them does not create its page or start background work. Disabled add-ons
have no settings entries. Disabling keeps data, preferences and accounts. All
pages can be off; core Settings and add-on management remain available.

## Update, remove and recover

Externally replacing or deleting an already discovered valid package requires
restart; Pilon keeps its current snapshot until then. A changed package cannot replace
code under live objects. Shipped updates preserve locally modified packages and
do not restore deliberately removed ones.

Use the card's Remove action after disabling. Declared account cleanup must
succeed before removal; failed cleanup preserves retryable state and names the
owner. Removal records retained data ownership for a later reset. Restore missing
cleanup code if Pilon asks for it. Disabling alone does not sign out accounts.

If an add-on prevents normal startup, quit Pilon and run `pilon --disable-addons`.
Saved activation choices are preserved. This recovery mode cannot undo changes
that trusted code already made outside managed storage.

## Build a package

Follow the [authoring guide](index.md) and the
[Counter](../../examples/counter/readme.md) or
[Checklist](../../examples/checklist/readme.md) example. Ordinary external
packages need no Pilon source edit or application rebuild.
