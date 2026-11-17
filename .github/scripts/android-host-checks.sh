#!/usr/bin/env bash
set -euo pipefail

work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT

python3 src/tests/platform/android/android_packaging_tests.py
python3 src/tests/platform/android/android_pdf_sharing_tests.py \
    src/platform/android/src/org/pilon/PilonSharing.java --jdk "$JAVA_HOME"
python3 addons/assistant/scripts/android-runtime-smoke.py --self-test
cc -std=c17 -O2 -Wall -Wextra -Werror addons/assistant/platform/android/codex-dns.c -o "$work/dns"
python3 addons/assistant/platform/android/tests/test-codex-dns.py "$work/dns"

git clone --quiet --depth 1 --branch v6.12.0 https://github.com/qt/qtbase.git "$work/qtbase"
test "$(git -C "$work/qtbase" rev-parse HEAD)" = 025bdad181de241e81bf853c8a2d7bf3d19261a9
for patch in surface-lifecycle accessibility-activation ime-options; do
    patch --batch --fuzz=0 -p1 -d "$work/qtbase" -i "$PWD/src/platform/android/qt-platform/$patch.patch"
done
java_sources="$work/qtbase/src/android/jar/src/org/qtproject/qt/android"
python3 src/tests/platform/android/android_qt_ime_tests.py "$java_sources/QtInputConnection.java" \
    --jdk "$JAVA_HOME" --android-jar "$ANDROID_HOME/platforms/android-37.0/android.jar"
python3 src/tests/platform/android/android_qt_texture_resize_tests.py "$java_sources/QtTextureView.java" --jdk "$JAVA_HOME"
