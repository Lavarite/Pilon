# Android builds

Run these commands from the repository root. Pilon uses the same application
source and feature targets as Linux. `src/platform/android/` supplies shared
packaging and platform adapters; add-on-specific code, patches and recipes live
under `addons/<id>/platform/android/` and `addons/<id>/scripts/android/`.

```sh
bash src/scripts/android/build.sh --check
bash src/scripts/android/build.sh
```

The check reads the local toolchain without installing packages or building.
The full build prepares native runtimes, configures the Qt Android application,
packages a Debug APK by default, and checks its runtime payloads and 16 KB alignment.
To rebuild the app with existing staged runtimes:

```sh
PILON_BUILD_APP_ONLY=1 bash src/scripts/android/build.sh
```

Defaults are Qt 6.12.0 for host Linux and Android ARM64, JDK 21, SDK/build-tools
37 and NDK 27.2.12479018. Host and Android Qt versions must match. Override local
paths with `PILON_ANDROID_QT`, `QT_HOST_PATH`, `JAVA_HOME`, `ANDROID_SDK_ROOT` and
`ANDROID_NDK_ROOT`; `PILON_ANDROID_BUILD_DIR` changes the app build directory and
`PILON_BUILD_JOBS` controls build parallelism. Runtime recipes can download
sources/tooling and retain their pinned checksums and licenses.

The Debug APK is written to
`build/android/app/android-build/build/outputs/apk/debug/android-build-debug.apk`.
Building does not install the app or change a connected device.

## Release signing

Keep the release keystore and its private environment file outside the repository,
owned by the current user with mode `0600` or stricter. Configure
`PILON_ANDROID_KEYSTORE`, `PILON_ANDROID_KEY_ALIAS`,
`PILON_ANDROID_KEYSTORE_PASSWORD` and `PILON_ANDROID_KEY_PASSWORD` there:

```sh
PILON_ANDROID_BUILD_TYPE=Release PILON_RELEASE_ENV=/private/path/release.env \
  PILON_BUILD_APP_ONLY=1 bash src/scripts/android/build.sh
```

The script derives Microsoft's redirect path from the signing certificate,
aligns the APK, signs it and verifies its signature and payload. The result is
`builds/android/pilon-<version>-android-arm64-v8a.apk`. Provider registrations must match
that certificate. A release key cannot update a differently signed installation.

Build scripts load the ignored project `.env` and optional private release file;
inherited environment values take precedence. See
[Configuration and credentials](../../../docs/security.md) and
[release packaging](../../../docs/release.md). Packaging does not publish files
or establish that corresponding-source requirements are complete.

Android Codex sign-in still fails during device-code token polling. A signed APK
or successful build does not resolve that known issue.

## Runtime components

This directory owns the host build and shared document, Office and Qt recipes.
Feature builders live in `addons/<id>/scripts/android/`: Assistant owns inference,
audio and agent tools; Notes owns OCR; Git owns Git/provider helpers; SSH owns
network helpers; Devices owns Tailscale cleanup. For a focused rebuild, run the
owning add-on's builder. The host build orchestrates the required components.

Shared tests and instrumentation live in `src/tests/` and
`src/platform/android/tests/`. Feature probes live beside their implementation
under `addons/<id>/tests/` and `addons/<id>/platform/android/tests/`.
The host assembles these source inputs into generated build staging while
preserving their runtime paths. Edit the owning source, not generated copies.

Generated downloads, build trees and proof artifacts live in
`build/android/<component>/`. Deployable runtime files, Java libraries, assets
and third-party notices are staged in `builds/android/`. Neither belongs in
version control.
