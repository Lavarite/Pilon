#!/usr/bin/env bash
set -euo pipefail

pilon_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$pilon_root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
pilon_dev="${PILON_DEV_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/pilon-dev}"
pilon_work="$pilon_dev/dev-deps/android/media-runtime"
pilon_output="$pilon_dev/builds/android/runtime/arm64-v8a"
pilon_licenses="$pilon_dev/builds/android/licenses/media"
pilon_ndk="${ANDROID_NDK_ROOT:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}/ndk/27.2.12479018}"
pilon_llvm="$pilon_ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
[[ -x "$pilon_llvm/aarch64-linux-android28-clang" ]] || { printf 'Missing Android NDK compiler: %s\n' "$pilon_llvm" >&2; exit 1; }
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

download https://codeload.github.com/ggml-org/whisper.cpp/tar.gz/refs/tags/b4938 \
    "$pilon_work/downloads/whisper-b4938.tar.gz" \
    6d8d70a014ca2b10f8a6d006b8f423e5f5ef2afcfbe92b57ab4e01107238112a
download https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz \
    "$pilon_work/downloads/ffmpeg-9.0.2.tar.xz" \
    8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
[[ -f "$pilon_work/whisper.cpp-b4938/CMakeLists.txt" ]] || tar -xzf "$pilon_work/downloads/whisper-b4938.tar.gz" -C "$pilon_work"
[[ -f "$pilon_work/ffmpeg-9.0.2/configure" ]] || tar -xJf "$pilon_work/downloads/ffmpeg-9.0.2.tar.xz" -C "$pilon_work"

download https://github.com/ARM-software/kleidiai/releases/download/v1.24.0/kleidiai-v1.24.0-src.tar.gz \
    "$pilon_work/downloads/kleidiai-v1.24.0-src.tar.gz" \
    9348b969e042d8890a54b01a463dbe71f5a4c074b5329e9c26a85ef3b68aa19b
PILON_WHISPER_KLEIDIAI=ON bash "$pilon_root/addons/assistant/scripts/android/build-whisper-variants.sh"
pilon_whisper="$pilon_work/whisper-kleidiai-variants-build/bin"
# Keep the portable baseline and measured non-SVE paths; automatic SVE selection regressed on the phone.
for pilon_library in ggml-base ggml whisper ggml-cpu-android_armv8.0_1 \
        ggml-cpu-android_armv8.2_1 ggml-cpu-android_armv8.2_2 ggml-cpu-android_armv8.6_1; do
    install -m755 "$pilon_whisper/lib$pilon_library.so" "$pilon_output/lib$pilon_library.so.new"
    "$pilon_llvm/llvm-strip" --strip-unneeded "$pilon_output/lib$pilon_library.so.new"
done
# Publish dependencies before the helper, so no consumer sees a partially staged new runtime.
for pilon_library in ggml-base ggml whisper ggml-cpu-android_armv8.0_1 \
        ggml-cpu-android_armv8.2_1 ggml-cpu-android_armv8.2_2 ggml-cpu-android_armv8.6_1; do
    mv "$pilon_output/lib$pilon_library.so.new" "$pilon_output/lib$pilon_library.so"
done
(
    cd "$pilon_work/ffmpeg-9.0.2"
    ./configure --target-os=android --arch=aarch64 --enable-cross-compile \
        --cc="$pilon_llvm/aarch64-linux-android28-clang" --cxx="$pilon_llvm/aarch64-linux-android28-clang++" \
        --ar="$pilon_llvm/llvm-ar" --nm="$pilon_llvm/llvm-nm" --ranlib="$pilon_llvm/llvm-ranlib" --strip="$pilon_llvm/llvm-strip" \
        --enable-pic --enable-static --disable-shared --disable-doc --disable-debug --disable-autodetect --enable-zlib \
        --disable-ffplay --disable-network --disable-avdevice \
        --extra-ldflags="-Wl,-z,max-page-size=16384,-z,common-page-size=16384"
    make -j"${PILON_BUILD_JOBS:-4}" ffmpeg ffprobe
)
for pilon_pair in "whisper-cli:$pilon_whisper/whisper-cli" "ffmpeg:$pilon_work/ffmpeg-9.0.2/ffmpeg" "ffprobe:$pilon_work/ffmpeg-9.0.2/ffprobe"; do
    pilon_name="${pilon_pair%%:*}"
    install -m755 "${pilon_pair#*:}" "$pilon_output/libpilon_$pilon_name.so.new"
    "$pilon_llvm/llvm-strip" --strip-unneeded "$pilon_output/libpilon_$pilon_name.so.new"
    mv "$pilon_output/libpilon_$pilon_name.so.new" "$pilon_output/libpilon_$pilon_name.so"
done
cp "$pilon_work/whisper.cpp-b4938/LICENSE" "$pilon_licenses/whisper.cpp-LICENSE.txt"
cp "$pilon_work/ffmpeg-9.0.2/COPYING.LGPLv2.1" "$pilon_licenses/FFmpeg-LGPL-2.1.txt"
cp "$pilon_work/ffmpeg-9.0.2/LICENSE.md" "$pilon_licenses/FFmpeg-LICENSE.md"
cat > "$pilon_licenses/SOURCE.txt" <<'EOF'
whisper.cpp b4938: https://github.com/ggml-org/whisper.cpp/tree/b4938
Source SHA256: 6d8d70a014ca2b10f8a6d006b8f423e5f5ef2afcfbe92b57ab4e01107238112a
FFmpeg 9.0.2: https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz
Source SHA256: 8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
Archive signature verified with the FFmpeg release key published at https://ffmpeg.org/ffmpeg-devel.asc
Fingerprint: FCF986EA15E6E293A5644F10B4322F04D67658D8
Both engines are unmodified; exact build and relink instructions are addons/assistant/scripts/android/build-media-runtime.sh in Pilon's source.
Whisper uses guarded Android baseline, dotprod, FP16 and i8mm CPU backend modules with KleidiAI v1.24.0.
KleidiAI source SHA256: 9348b969e042d8890a54b01a463dbe71f5a4c074b5329e9c26a85ef3b68aa19b
https://github.com/ARM-software/kleidiai/tree/v1.24.0
KleidiAI Apache-2.0/BSD-3-Clause licenses and copyright notices are also bundled under licenses/runtime.
FFmpeg uses its LGPL configuration, native local-file decoders/PCM conversion and segmenter. No network or capture-device backend is enabled; Pilon owns download and microphone access.
EOF
