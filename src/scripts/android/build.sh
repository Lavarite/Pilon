#!/usr/bin/env bash
set -euo pipefail

pilon_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$pilon_root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
pilon_sdk="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
pilon_ndk="${ANDROID_NDK_ROOT:-$pilon_sdk/ndk/27.2.12479018}"
pilon_jdk="${JAVA_HOME:-$HOME/Android/jdk-21}"
pilon_qt="${PILON_ANDROID_QT:-$HOME/Qt/6.12.0/android_arm64_v8a}"
pilon_host_qt="${QT_HOST_PATH:-$HOME/Qt/6.12.0/gcc_64}"
pilon_build="${PILON_ANDROID_BUILD_DIR:-$pilon_root/build/android/app}"
pilon_mode="${PILON_ANDROID_BUILD_TYPE:-Debug}"
case "$pilon_mode" in
    Debug|Release) ;;
    *) printf 'PILON_ANDROID_BUILD_TYPE must be Debug or Release.\n' >&2; exit 2 ;;
esac

if [[ $# -gt 1 || ( $# -eq 1 && "$1" != --check ) ]]; then
    printf 'Usage: %s [--check]\n' "$0" >&2
    exit 2
fi
for pilon_program in cmake ninja python3 perl make; do
    command -v "$pilon_program" >/dev/null || { printf 'Missing build tool: %s\n' "$pilon_program" >&2; exit 1; }
done
for pilon_file in "$pilon_qt/bin/qt-cmake" "$pilon_qt/bin/qmake" "$pilon_host_qt/bin/qmake" \
    "$pilon_ndk/build/cmake/android.toolchain.cmake" "$pilon_jdk/bin/javac" \
    "$pilon_sdk/platforms/android-37.0/android.jar" "$pilon_sdk/build-tools/37.0.0/apksigner"; do
    [[ -f "$pilon_file" ]] || { printf 'Missing Android prerequisite: %s\n' "$pilon_file" >&2; exit 1; }
done
[[ "$("$pilon_qt/bin/qmake" -query QT_VERSION)" == "$("$pilon_host_qt/bin/qmake" -query QT_VERSION)" ]] || {
    printf 'Host and Android Qt versions must match.\n' >&2; exit 1;
}
if [[ "$pilon_mode" == Release ]]; then
    set +x
    for pilon_name in PILON_ANDROID_KEYSTORE PILON_ANDROID_KEY_ALIAS PILON_ANDROID_KEYSTORE_PASSWORD PILON_ANDROID_KEY_PASSWORD; do
        [[ -n "${!pilon_name:-}" ]] || { printf 'Missing release setting: %s\n' "$pilon_name" >&2; exit 1; }
        export "$pilon_name"
    done
    PILON_ANDROID_KEYSTORE="$(python3 -c 'import os; from pathlib import Path; print(Path(os.environ["PILON_ANDROID_KEYSTORE"]).expanduser().resolve(strict=True))')"
    export ORG_GRADLE_PROJECT_pilonMicrosoftRedirectPath
    ORG_GRADLE_PROJECT_pilonMicrosoftRedirectPath="$(python3 - "$pilon_root" "$pilon_jdk" <<'PY'
import base64, hashlib, os
from pathlib import Path
import stat, subprocess, sys
root, jdk = map(Path, sys.argv[1:])
key = Path(os.environ['PILON_ANDROID_KEYSTORE']).expanduser().resolve(strict=True)
if key.is_relative_to(root) or key.stat().st_uid != os.getuid() or stat.S_IMODE(key.stat().st_mode) & 0o077:
    raise SystemExit('The release keystore must be outside the repository, owned by this user, with mode 0600 or stricter')
certificate = subprocess.check_output([str(jdk/'bin/keytool'), '-exportcert', '-keystore', str(key),
    '-alias', os.environ['PILON_ANDROID_KEY_ALIAS'], '-storepass:env', 'PILON_ANDROID_KEYSTORE_PASSWORD'])
print('/' + base64.b64encode(hashlib.sha1(certificate).digest()).decode())
PY
)"
fi
if [[ "${1:-}" == --check ]]; then
    printf 'Android build prerequisites verified.\n'
    exit 0
fi
export JAVA_HOME="$pilon_jdk"
export PATH="$JAVA_HOME/bin:$PATH"
python3 "$pilon_root/src/tests/platform/android/android_pdf_sharing_tests.py" \
    "$pilon_root/src/platform/android/src/org/pilon/PilonSharing.java" --jdk "$pilon_jdk"
# Android's standard debug certificate is also used by the MSAL manifest redirect.
if [[ "$pilon_mode" == Debug && ! -f "$HOME/.android/debug.keystore" ]]; then
    mkdir -p "$HOME/.android"
    keytool -genkeypair -keystore "$HOME/.android/debug.keystore" -storepass android -keypass android \
        -alias androiddebugkey -dname 'CN=Android Debug,O=Android,C=US' -keyalg RSA -keysize 2048 -validity 10000
fi
if [[ "${PILON_BUILD_APP_ONLY:-0}" != 1 ]]; then
    ANDROID_NDK_ROOT="$pilon_ndk" "$pilon_root/addons/assistant/scripts/android/build-runtime.sh"
    ANDROID_NDK_ROOT="$pilon_ndk" "$pilon_root/addons/notes/scripts/android/build-ocr.sh"
    bash "$pilon_root/src/scripts/android/build-pdfium.sh"
    bash "$pilon_root/src/scripts/android/build-office.sh"
    bash "$pilon_root/addons/assistant/scripts/android/build-media-runtime.sh"
    bash "$pilon_root/addons/assistant/scripts/android/build-breeze.sh"
    bash "$pilon_root/addons/git/scripts/android/build-git-providers.sh"
    bash "$pilon_root/addons/devices/scripts/android/build-tailscale.sh"
    bash "$pilon_root/src/scripts/android/build-qt-platform.sh"
    bash "$pilon_root/addons/assistant/scripts/android/build-agent-runtime.sh"
fi
"$pilon_qt/bin/qt-cmake" -S "$pilon_root/src" -B "$pilon_build" -GNinja \
    -DCMAKE_BUILD_TYPE=Release -DQT_ANDROID_DEPLOYMENT_TYPE="$pilon_mode" -DPILON_BUILD_TESTS=OFF \
    -DQT_HOST_PATH="$pilon_host_qt" -DANDROID_SDK_ROOT="$pilon_sdk" -DANDROID_NDK_ROOT="$pilon_ndk"
cmake --build "$pilon_build" --target apk --parallel "${PILON_BUILD_JOBS:-4}"
if [[ "$pilon_mode" == Release ]]; then
    pilon_unsigned="$pilon_build/android-build/build/outputs/apk/release/android-build-release-unsigned.apk"
    pilon_version="$(sed -n 's/^project(Pilon VERSION \([0-9.]*\) .*/\1/p' "$pilon_root/src/CMakeLists.txt")"
    pilon_apk="$pilon_root/builds/android/pilon-$pilon_version-android-arm64-v8a.apk"
    mkdir -p "$(dirname "$pilon_apk")"
    pilon_aligned="$(mktemp "$pilon_root/builds/android/.aligned.XXXXXX.apk")"
    trap 'rm -f -- "$pilon_aligned" "$pilon_apk.new"' EXIT
    "$pilon_sdk/build-tools/37.0.0/zipalign" -f -P 16 4 "$pilon_unsigned" "$pilon_aligned"
    "$pilon_sdk/build-tools/37.0.0/apksigner" sign --ks "$PILON_ANDROID_KEYSTORE" \
        --ks-key-alias "$PILON_ANDROID_KEY_ALIAS" --ks-pass env:PILON_ANDROID_KEYSTORE_PASSWORD \
        --key-pass env:PILON_ANDROID_KEY_PASSWORD --out "$pilon_apk.new" "$pilon_aligned"
    "$pilon_sdk/build-tools/37.0.0/apksigner" verify "$pilon_apk.new"
    python3 "$pilon_root/addons/assistant/scripts/android-runtime-smoke.py" --check-apk "$pilon_apk.new"
    "$pilon_sdk/build-tools/37.0.0/zipalign" -c -P 16 4 "$pilon_apk.new"
    mv -f -- "$pilon_apk.new" "$pilon_apk"
else
    pilon_apk="$pilon_build/android-build/build/outputs/apk/debug/android-build-debug.apk"
    python3 "$pilon_root/addons/assistant/scripts/android-runtime-smoke.py" --check-apk "$pilon_apk"
    "$pilon_sdk/build-tools/37.0.0/zipalign" -c -P 16 4 "$pilon_apk"
fi
printf 'Android %s APK: %s\n' "$pilon_mode" "$pilon_apk"
