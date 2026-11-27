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
sources and tools and retain their pinned checksums and licenses.

The development root defaults to `${XDG_CACHE_HOME:-$HOME/.cache}/pilon-dev`.
Set `PILON_DEV_DIR` to an absolute path to override it. The default Debug APK
relative to that root is
`builds/work/android/app/android-build/build/outputs/apk/debug/android-build-debug.apk`.
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
aligns the APK, signs it and verifies its signature and payload. It writes
`builds/android/pilon-<version>-android-arm64-v8a.apk` under the development root.
Provider registrations must match
that certificate. A release key cannot update a differently signed installation.

Build scripts load the ignored project `.env` and optional private release file.
Inherited environment values take precedence. [Configuration and credentials](../../../docs/security.md)

To create local packaging assets from the signed APK, run:

```sh
python3 src/scripts/package-release.py android --corresponding-source \
  --android-apk /path/to/pilon-VERSION-android-arm64-v8a.apk
```

Verify the generated checksums and resolve any missing corresponding source
before distributing the APK. [Third-party licenses](../../../docs/third-party.md)

See the [Android installation guide](../../../docs/install.md#android) for preview limitations.

## Runtime components

This directory owns the host build and shared document, Office and Qt recipes.
Feature builders live in `addons/<id>/scripts/android/`. Assistant owns inference,
audio and agent tools. Notes owns OCR, SSH owns network helpers, and Devices owns
Tailscale. For a focused rebuild, run the owning add-on's builder. The host build
orchestrates the required components.

Android upgrades Codex with the signed APK. To prepare a new version, update the
tag version, commit and archive SHA-256 in the [Codex release pin](../../../addons/assistant/scripts/android/codex-release.toml).
Run `bash addons/assistant/scripts/android/build-codex.sh` before packaging. The
builder takes the Rust version from the pinned upstream source and checks the
Android shell and DNS patch against it. Verify native shell execution with
`python3 addons/assistant/tests/android/android_codex_shell_smoke.py --device <adb-serial>`.

Shared tests and instrumentation live in `src/tests/` and
`src/platform/android/tests/`. Feature probes live beside their implementation
under `addons/<id>/tests/` and `addons/<id>/platform/android/tests/`.
The host assembles these source inputs into generated build staging while
preserving their runtime paths. Edit the owning source, not generated copies.

Under the development root, app builds and standalone probes use
`builds/work/android/`. Native dependency recipes use
`dev-deps/android/<component>/` and `dev-deps/downloads/` for their local inputs.
Deployable libraries, assets and third-party notices use `builds/android/`.
