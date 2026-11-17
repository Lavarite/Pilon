# SSH

Enable SSH in **Settings → Add-ons**, then open its sidebar page. Set the host,
user and port before connecting. The page provides a remote terminal; connection
defaults are configured in the add-on's settings.

Verify an unknown host fingerprint with the server owner before accepting it.
A changed host key must be investigated rather than accepted automatically.
Saved passwords use the platform credential store. Disconnection closes the
session; disabling the add-on preserves its saved configuration and accounts.

Devices supplied by the Devices add-on can be selected when that integration is
enabled. Direct SSH hosts do not require Tailscale. File browsing and transfers
are provided by the Files add-on.

## Source layout

`src/` and `package/` contain the native implementation and terminal/page
entries. `platform/android/` owns SSH patches, `scripts/android/` builds network
helpers, and `tests/` contains connection/input probes. Shared host packaging
assembles the Android inputs. See [Contributing](../../docs/CONTRIBUTING.md)
for checks and [API 1](../../docs/addons/API.md) for ownership and cleanup.
