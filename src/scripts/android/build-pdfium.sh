#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
version=8138
checksum=fc73c8020d7204e52f7a9d8153489b3cac2e9c5dc5d70d3a37a976a7398c1a86
archive="$root/downloads/pdfium-android-arm64-$version.tgz"
sdk="$root/build/android/pdfium/sdk"
runtime="$root/builds/android/runtime/arm64-v8a"
licenses="$root/builds/android/licenses/pdfium"
mkdir -p "$(dirname "$archive")" "$sdk" "$runtime" "$licenses"
if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 3 "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/$version/pdfium-android-arm64.tgz" -o "$archive.new"
    mv "$archive.new" "$archive"
fi
printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check --status
tar -xzf "$archive" -C "$sdk"
cp "$sdk/lib/libpdfium.so" "$runtime/libpdfium.so.new"
mv "$runtime/libpdfium.so.new" "$runtime/libpdfium.so"
cp "$sdk/LICENSE" "$licenses/LICENSE"
cp -R "$sdk/licenses/." "$licenses/"
cat > "$licenses/SOURCE.txt" <<EOF
PDFium 159.0.8138.0, non-V8 Android ARM64 build
Source: https://pdfium.googlesource.com/pdfium/+/refs/heads/chromium/8138
Build: https://github.com/bblanchon/pdfium-binaries/releases/tag/chromium/8138
Archive SHA256: $checksum
EOF
printf 'PDFium headers: %s/include\nPDFium runtime: %s/libpdfium.so\n' "$sdk" "$runtime"
