# Devices

Enable Devices in Add-ons, then open its settings to choose the devices available
in Files and SSH. This device remains available independently of Devices.

## Android

Pilon uses the official [Tailscale Android app](https://tailscale.com/docs/install/android)
for sign-in and the VPN. Choose **Install Tailscale** if needed, then **Open
Tailscale** to sign in. **Connect** requests the official app's VPN and opens it
for any required approval. **Disconnect** requests disconnection without signing
out. Pilon does not create a second Tailscale account or VPN.

In **Saved devices**, choose **Add device**, enter a name and its Tailscale IP
address or MagicDNS name, then save. IPv4, IPv6 and DNS names are supported. The
checkbox includes the saved destination in Files and SSH. Its action menu edits
or removes it. SSH accounts, credentials and host-key checks use the existing
connection controls.

The official app does not expose its peer list or account status to other apps.
Pilon therefore saves destinations explicitly. Their reachability stays unknown
until a connection is attempted; a saved destination is never marked online
merely because a VPN exists. **Android VPN active** reports Android's network
transport, which can belong to another VPN. Check Tailscale itself for its
connection status. **No Android VPN active** means Android exposes no VPN to
Pilon. If Pilon is excluded through
Tailscale's app routing, enable it there before connecting to tailnet addresses.

Disabling Devices stops its runtime; it does not sign out of the official app.
Removing a saved destination affects only Pilon's list. Cleanup of a previous
Pilon-owned Tailscale session affects that old private session, never the official
app's account.

## Linux

Pilon uses the installed Tailscale CLI and system service. Start the service and
sign in when required. **Use in Pilon** confirms use of an existing system
session. Select included devices with their checkboxes. Signing out affects
other applications that use that system Tailscale session.

## Source layout

`package/` and `src/` contain the package and native implementation.
`platform/android/` owns the Android Tailscale bridge, patches and instrumentation;
`scripts/android/` builds its helper, and `tests/` contains feature tests.
Shared host packaging assembles the Android inputs.
