# Tailscale

Enable Tailscale in Add-ons to discover peers. Open **SSH → Tailscale** to
manage its connection. Choose which peers appear in SSH and Files under
**SSH → Connections**. Saved peers appear during refresh. A successful
discovery makes them available for connections. Direct SSH hosts work independently.

## Android

Pilon uses the official [Tailscale Android app](https://tailscale.com/docs/install/android)
for sign-in and the VPN. Choose **Install Tailscale** if needed, then **Open
Tailscale** to sign in. **Connect** requests the official app's VPN and opens it
for any required approval. **Disconnect** requests disconnection without signing
out. Pilon does not create a second Tailscale account or VPN.

Without a discovery credential, save tailnet addresses in **SSH → Connections**.
A saved destination's reachability stays unknown until a connection is attempted. **Android VPN active** reports Android's network
transport, which can belong to another VPN. Check Tailscale itself for its
connection status. **No Android VPN active** means Android exposes no VPN to
Pilon. If Pilon is excluded through
Tailscale's app routing, enable it there before connecting to tailnet addresses.

Disabling Tailscale stops discovery and leaves the official app signed in. Cleanup of a previous
Pilon-owned Tailscale session affects that old private session, never the official
app's account.

## Linux

Pilon uses the installed Tailscale CLI and system service. Start the service and
sign in when required. **Use in Pilon** confirms use of an existing system
session. Select the peers to include in SSH settings. Signing out affects
other applications that use that system Tailscale session.

## Source layout

`package/` and `src/` contain the package and native implementation.
`platform/android/` owns the Android Tailscale bridge, patches and instrumentation;
`scripts/android/` builds its helper, and `tests/` contains feature tests.
Shared host packaging assembles the Android inputs.
