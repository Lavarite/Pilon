# SSH

1. Enable SSH in **Settings → Add-ons**.
2. Open **SSH → Connections** and add a host with its username. On Linux, an OpenSSH alias can retain its configured port and identity.
3. Open the SSH page and connect. Verify an unfamiliar fingerprint with the server owner before accepting it.

Saved passwords use the platform credential store. Ctrl+C copies selected terminal text or interrupts the remote command when nothing is selected.

Use Files for SFTP. Use the SSH terminal for aliases that require a proxy or jump host. Selected [Tailscale peers](../devices/readme.md) can appear in both.

## X11 forwarding

Enable **Trusted X11 forwarding** in SSH connection settings for a new interactive terminal. It needs a working local X display and a server that permits forwarding. Trusted remote apps can read and control that display.

## Disable SSH

Disabling SSH closes access to remote devices and preserves saved connections. Local Files stays available.
