#!/usr/bin/env bash
set -euo pipefail
pilon_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$pilon_root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
pilon_work="$pilon_root/build/android/ocr"
pilon_output="$pilon_root/builds/android/runtime/arm64-v8a"
pilon_licenses="$pilon_root/builds/android/licenses/ocr"
pilon_ndk="${ANDROID_NDK_ROOT:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}/ndk/27.2.12479018}"
pilon_qt="${PILON_ANDROID_QT:-$HOME/Qt/6.12.0/android_arm64_v8a}"
pilon_host_qt="${QT_HOST_PATH:-$HOME/Qt/6.12.0/gcc_64}"
pilon_rapid_revision=5e675ed23b2e5f87acd264b4aa0948f324d05ec8
# 1.23.2 selects unsupported SME2 instructions on the connected SM8850.
pilon_ort_version=1.31.1
pilon_ort="$pilon_work/onnxruntime-$pilon_ort_version"
mkdir -p "$pilon_work/downloads" "$pilon_output" "$pilon_licenses"

download() {
    local pilon_url="$1" pilon_file="$2" pilon_sha="$3"
    if [[ ! -f "$pilon_file" ]]; then
        curl --fail --location --retry 3 --output "$pilon_file.part" "$pilon_url"
        printf '%s  %s\n' "$pilon_sha" "$pilon_file.part" | sha256sum --check
        mv "$pilon_file.part" "$pilon_file"
    fi
    printf '%s  %s\n' "$pilon_sha" "$pilon_file" | sha256sum --check
}
download "https://codeload.github.com/RapidAI/RapidOcrAndroidOnnx/tar.gz/$pilon_rapid_revision" \
    "$pilon_work/downloads/rapid-$pilon_rapid_revision.tar.gz" \
    6e5df307d1c3edcdbfb85ff32764e152c2248d560e9253c7de2c42dff4e46d8e
# Official Microsoft Maven artifact; its published SHA-1 was cross-checked when pinning this SHA-256.
download "https://repo.maven.apache.org/maven2/com/microsoft/onnxruntime/onnxruntime-android/$pilon_ort_version/onnxruntime-android-$pilon_ort_version.aar" \
    "$pilon_work/downloads/onnxruntime-android-$pilon_ort_version.aar" \
    d4d50660300ed2c4f38ffdfd5ab5bfa3a22f995f282dda24f42ea3ea55467b49
download https://github.com/opencv/opencv/releases/download/4.12.0/opencv-4.12.0-android-sdk.zip \
    "$pilon_work/downloads/opencv-4.12.0-android-sdk.zip" \
    fd7f2332331b4eb8b67e55137281cfb16823c9399d90deb9cfa3476783b99e35
download "https://raw.githubusercontent.com/microsoft/onnxruntime/v$pilon_ort_version/LICENSE" \
    "$pilon_work/downloads/onnxruntime-LICENSE.txt" \
    2f07c72751aed99790b8a4869cf2311df85a860b22ded05fa22803587a48922c
download "https://raw.githubusercontent.com/microsoft/onnxruntime/v$pilon_ort_version/ThirdPartyNotices.txt" \
    "$pilon_work/downloads/onnxruntime-ThirdPartyNotices.txt" \
    143764b952fdb1a7c69ce653bfba74a7744d6a8a573bfb73e235fba356c83de3
download https://raw.githubusercontent.com/RapidAI/RapidOCR/v3.9.2/LICENSE \
    "$pilon_work/downloads/RapidOCR-LICENSE.txt" \
    3e0af25fdd06aa9586ae97adb00ea927ebe5a3805ac77d2d3a81ce5f55693333

[[ -d "$pilon_work/RapidOcrAndroidOnnx-$pilon_rapid_revision" ]] \
    || tar -xzf "$pilon_work/downloads/rapid-$pilon_rapid_revision.tar.gz" -C "$pilon_work"
[[ -d "$pilon_ort/headers" ]] \
    || unzip -q "$pilon_work/downloads/onnxruntime-android-$pilon_ort_version.aar" -d "$pilon_ort"
[[ -d "$pilon_work/OpenCV-android-sdk" ]] \
    || unzip -q "$pilon_work/downloads/opencv-4.12.0-android-sdk.zip" -d "$pilon_work"

"$pilon_qt/bin/qt-cmake" -S "$pilon_root/addons/notes/platform/android/ocr" -B "$pilon_work/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DQT_HOST_PATH="$pilon_host_qt" -DANDROID_NDK_ROOT="$pilon_ndk" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_shared \
    -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
    -DPILON_RAPID="$pilon_work/RapidOcrAndroidOnnx-$pilon_rapid_revision" \
    -DPILON_ORT="$pilon_ort" -DOpenCV_DIR="$pilon_work/OpenCV-android-sdk/sdk/native/jni"
cmake --build "$pilon_work/build" --target pilon_ocr --parallel "${PILON_BUILD_JOBS:-4}"
install -m755 "$pilon_work/build/libpilon_ocr.so" "$pilon_output/libpilon_ocr.so.new"
"$pilon_ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded "$pilon_output/libpilon_ocr.so.new"
mv "$pilon_output/libpilon_ocr.so.new" "$pilon_output/libpilon_ocr.so"
install -m755 "$pilon_ort/jni/arm64-v8a/libonnxruntime.so" "$pilon_output/libonnxruntime.so.new"
mv "$pilon_output/libonnxruntime.so.new" "$pilon_output/libonnxruntime.so"
cp "$pilon_root/addons/notes/platform/android/ocr/NOTICE.txt" "$pilon_licenses/NOTICE.txt"
cp "$pilon_work/RapidOcrAndroidOnnx-$pilon_rapid_revision/LICENSE" "$pilon_licenses/RapidOcrAndroidOnnx-LICENSE.txt"
cp "$pilon_work/downloads/onnxruntime-LICENSE.txt" "$pilon_work/downloads/onnxruntime-ThirdPartyNotices.txt" \
    "$pilon_work/downloads/RapidOCR-LICENSE.txt" "$pilon_licenses/"
cp "$pilon_work/OpenCV-android-sdk/LICENSE" "$pilon_licenses/OpenCV-LICENSE.txt"
cp -R "$pilon_work/OpenCV-android-sdk/sdk/etc/licenses" "$pilon_licenses/OpenCV-components"
sed -n '1,24p' "$pilon_work/RapidOcrAndroidOnnx-$pilon_rapid_revision/OcrLibrary/src/main/cpp/include/clipper.hpp" \
    > "$pilon_licenses/Clipper-LICENSE.txt"
printf 'Android PP-OCRv5 helper: %s\nLicenses: %s\n' "$pilon_output/libpilon_ocr.so" "$pilon_licenses"
