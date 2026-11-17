# Installation

## Linux

The x86_64 archive uses system Qt and shared libraries. Check the minimum
versions in the release notes. On a current Arch Linux install, the base
runtime packages are `qt6-base`, `qt6-declarative`, `qt6-multimedia`,
`qt6-svg` and `libssh2`. Use `qt6-wayland` for a native Wayland session.

The [installer command](../README.md#install) uses Python 3, checks the release
archive's SHA-256, installs under `~/.local/opt/pilon` and adds Pilon to your
application menu. It installs for the current user.

For a manual install, download the archive and matching `SHA256SUMS` from
[GitHub Releases](https://github.com/Lavarite/Pilon/releases). In the download folder, run:

```sh
sha256sum --check --ignore-missing pilon-*-linux-x86_64-SHA256SUMS &&
tar -xzf pilon-*-linux-x86_64.tar.gz &&
cd pilon-*-linux-x86_64 &&
./builds/pilon
```

Keep the extracted folder together. To add Pilon to your application menu,
place the folder where you want to keep it, then run
`python3 src/scripts/install-desktop-entry.py` from inside it.

If your system libraries are older than the release requires, see
[Build from source](CONTRIBUTING.md#build-from-source). Model weights and optional
Python and Office runtimes are separate. See [optional runtimes](CONTRIBUTING.md#optional-runtimes)
and **Settings → Models**.

## Android

The preview APK requires Android 9 or later on an ARM64 device. Download
`pilon-*-android-arm64-v8a.apk` from [GitHub Releases](https://github.com/Lavarite/Pilon/releases)
to the device and open it. If Android prompts, allow installation from the
browser or file manager that opened the APK.

To verify the APK on Linux before copying it to the device:

```sh
sha256sum --check --ignore-missing pilon-*-android-arm64-v8a-SHA256SUMS
```

Updates require the same signing certificate. Back up your Pilon data before
replacing a differently signed installation.

Native ARM64 device validation is pending. On the x86_64 Android emulator,
ARM translation crashes in ONNX Runtime during startup. Codex device-code
sign-in currently fails during token polling.

See the [Android build guide](../src/scripts/android/README.md) for building
and signing an APK.

## First run

Choose a writable Pilon workspace for notes, calendar files and generated
artifacts. Use Pilon's storage settings to move existing data. Account
credentials use the platform credential store.

Enable built-in features or import trusted `.piext` packages in
**Settings → Add-ons**. See [add-on installation and recovery](addons/install.md)
and [feature guides](index.md#add-ons).
