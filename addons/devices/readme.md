# Tailscale

Enable Tailscale in **Settings → Add-ons**. Use **SSH → Tailscale** to manage the connection and **SSH → Connections** to select peers for SSH and Files.

## Android

1. Install the [official Tailscale app](https://tailscale.com/docs/install/android).
2. Choose **Open Tailscale** to sign in.
3. Choose **Connect** and approve the VPN in Tailscale if prompted.

Use **Disconnect** to stop the VPN. Disabling the Pilon add-on stops discovery and leaves the official app signed in. Check the connection in Tailscale if Android reports a VPN but a peer fails to open.

To use a peer without discovery, save its tailnet address in **SSH → Connections**. Pilon tests reachability when you connect.

## Linux

Start the Tailscale service and sign in. Choose **Use in Pilon** to use the existing system session, then select the peers to include. Signing out affects other apps that use that session.

[Files and transfers](../files/readme.md) · [SSH connections](../ssh/readme.md)
