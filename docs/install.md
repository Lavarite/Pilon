# Install Pilon

Use [GitHub Releases](https://github.com/Lavarite/Pilon/releases) for platform
downloads, checksums, and source bundles.

## Linux

Install `curl`, `python3`, and the runtime libraries listed in the release notes.
For the Linux x86-64 release archive, run the terminal installer.

```sh
curl -fsSL https://pilon.vasylevskyi.net/download | sh
```

The installer verifies the download, installs to `~/Pilon` and adds Pilon to
your application menu. Set `PILON_INSTALL_ROOT` to choose another location.

<details>
<summary>Install a Linux archive manually</summary>

Download the archive and matching `SHA256SUMS` from the same release. Run these
commands from their download folder.

```sh
sha256sum --check --ignore-missing pilon-*-linux-x86_64-SHA256SUMS &&
tar -xzf pilon-*-linux-x86_64.tar.gz
```

Open `.bin/pilon` inside the extracted folder. To add a launcher there, run
`python3 src/scripts/install-desktop-entry.py`.

On Arch, the system libraries for the manual archive are `qt6-base`,
`qt6-declarative`, `qt6-multimedia`, `qt6-svg` and `libssh2`.
Install `qt6-wayland` for native Wayland.

</details>

If your system libraries are too old, [build from source](../CONTRIBUTING.md#build-from-source).
See [optional runtimes](../CONTRIBUTING.md#optional-runtimes) for previews,
handwriting and local models.

## Android

On ARM64 devices running Android 9 or later, open the signed APK from the
release assets when available. Use the same signing certificate to update an
existing installation. See the [Android build guide](../src/scripts/android/README.md)
to build an APK from source.

To install a published APK from Termux, install `curl` and `python` with `pkg`
and run the terminal installer. It verifies the APK before opening it.

Google Calendar sign-in is available on Linux. See the
[Calendar guide](../addons/calendar/readme.md) for supported Android accounts.

Startup and Files were tested on an Android 15 emulator with ARM64 translation.
Native ARM64 device testing is pending. Codex device-code sign-in currently fails
during token polling.

## First run

Choose a writable workspace. Android starts with a private workspace.
Use **Settings → Add-ons** to enable features or import a trusted `.piext`.

| Add-on | Workspace folder |
| --- | --- |
| Notes | `notes/` when Notes first saves data |
| Calendar | `calendar/` when Calendar starts |
| Assistant | `artifacts/` when Assistant saves its first artifact |

Only enabled add-ons create their folders. Disabling an add-on keeps its existing
folders and files. [Add-on setup](addons/install.md) · [Workspace settings](configuration.md)
