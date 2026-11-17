# Releases

## GitHub release

Tag the verified commit on `main` with `vMAJOR.MINOR.PATCH`, matching the
version in [CMake](../src/CMakeLists.txt). The [CI workflow](../.github/workflows/ci.yml)
runs on the tag and stages the source and Linux downloads in a draft GitHub
release. It checks the Linux corresponding-source report before uploading.

## Android preview

Use the [Android build guide](../src/scripts/android/README.md) to build a signed
ARM64 APK. Keep the release key outside the checkout and use the same
certificate for updates. On a prepared release workstation, from the checkout
root:

```sh
version=1.0.0
assets="$PWD/builds/releases/$version"
python3 src/scripts/package-release.py android --corresponding-source \
  --android-apk "builds/android/pilon-$version-android-arm64-v8a.apk" \
  --output-dir "$assets"
(cd "$assets" && sha256sum --check "pilon-$version-android-arm64-v8a-SHA256SUMS")
```

Confirm the APK signature, alignment and matching source assets. Review every
`missing` entry in the corresponding-source report before distributing the APK.
Smoke-test installation and startup on a native ARM64 device, and record any
coverage gaps in the draft release. Upload the APK, checksums, notices, source
assets and their report together:

```sh
gh release upload "v$version" "$assets"/pilon-"$version"-android-*
```

## Publish

Verify the main download links and checksums. List Linux runtime requirements
and Android preview limitations in the release notes. Include matching source
and license materials with both binaries. See [installation](install.md) for the
user-facing steps.
