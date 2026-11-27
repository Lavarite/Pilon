#!/usr/bin/env bash
set -euo pipefail

pilon_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$pilon_root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
pilon_dev="${PILON_DEV_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/pilon-dev}"
pilon_work="$pilon_dev/dev-deps/android/runtime"
pilon_output="$pilon_dev/builds/android/runtime/arm64-v8a"
pilon_ndk="${ANDROID_NDK_ROOT:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}/ndk/27.2.12479018}"
pilon_llama_revision=4d9176092d00586775af140581bb0b558ddc4389
pilon_kleidiai_version=v1.24.0
pilon_licenses="$pilon_dev/builds/android/licenses/runtime"
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

# The Android-only source patch fixes Codex's default /bin/sh fallback.
bash "$pilon_root/addons/assistant/scripts/android/build-codex.sh"

download "https://codeload.github.com/ggml-org/llama.cpp/tar.gz/$pilon_llama_revision" \
    "$pilon_work/downloads/llama-$pilon_llama_revision.tar.gz" \
    1a76abb8f7fe887c9cd37f71637b1a75334340e4e7fad38ef9b15d1a47ac511a
download "https://github.com/ARM-software/kleidiai/releases/download/$pilon_kleidiai_version/kleidiai-$pilon_kleidiai_version-src.tar.gz" \
    "$pilon_work/downloads/kleidiai-$pilon_kleidiai_version-src.tar.gz" \
    9348b969e042d8890a54b01a463dbe71f5a4c074b5329e9c26a85ef3b68aa19b
install -m644 "$pilon_licenses/codex/LICENSE.txt" "$pilon_licenses/Codex-LICENSE.txt"
install -m644 "$pilon_licenses/codex/NOTICE.txt" "$pilon_licenses/Codex-NOTICE.txt"
pilon_kleidiai_source="$pilon_work/kleidiai-$pilon_kleidiai_version"
if [[ ! -f "$pilon_kleidiai_source/CMakeLists.txt" ]]; then
    tar -xzf "$pilon_work/downloads/kleidiai-$pilon_kleidiai_version-src.tar.gz" -C "$pilon_work"
fi

if [[ ! -f "$pilon_ndk/build/cmake/android.toolchain.cmake" ]]; then
    printf 'Android NDK 27.2.12479018 is required: %s\n' "$pilon_ndk" >&2
    exit 1
fi
pilon_source="$pilon_work/llama.cpp-$pilon_llama_revision"
if [[ ! -f "$pilon_source/.pilon-prefix-cache.patch" ]] \
    || ! cmp -s "$pilon_root/addons/assistant/config/llama-prefix-cache.patch" "$pilon_source/.pilon-prefix-cache.patch"; then
    rm -rf -- "$pilon_source"
    tar -xzf "$pilon_work/downloads/llama-$pilon_llama_revision.tar.gz" -C "$pilon_work"
    patch -d "$pilon_source" -p1 < "$pilon_root/addons/assistant/config/llama-prefix-cache.patch"
    cp "$pilon_root/addons/assistant/config/llama-prefix-cache.patch" "$pilon_source/.pilon-prefix-cache.patch"
fi

cp "$pilon_source/LICENSE" "$pilon_licenses/llama.cpp-LICENSE.txt"
cp "$pilon_kleidiai_source/LICENSES/Apache-2.0.txt" "$pilon_licenses/KleidiAI-Apache-2.0.txt"
cp "$pilon_kleidiai_source/LICENSES/BSD-3-Clause.txt" "$pilon_licenses/KleidiAI-BSD-3-Clause.txt"
python3 - "$pilon_kleidiai_source" "$pilon_licenses/KleidiAI-NOTICE.txt" <<'PY'
from pathlib import Path
import sys
notices = set()
for path in Path(sys.argv[1]).rglob('*'):
    if path.is_file():
        for line in path.read_text(errors='ignore').splitlines():
            if 'SPDX-FileCopyrightText:' in line:
                notices.add(line.split('SPDX-FileCopyrightText:', 1)[1].strip())
Path(sys.argv[2]).write_text('KleidiAI v1.24.0\nhttps://github.com/ARM-software/kleidiai\n\n' + '\n'.join(sorted(notices)) + '\n')
PY

# APK native helpers run from nativeLibraryDir; writable model/data folders hold no executable code.
cmake -S "$pilon_source" -B "$pilon_work/llama-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$pilon_ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_static \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,-z,max-page-size=16384,-z,common-page-size=16384" \
    -DBUILD_SHARED_LIBS=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF \
    -DLLAMA_BUILD_TOOLS=ON -DLLAMA_OPENSSL=OFF -DLLAMA_CURL=OFF \
    -DLLAMA_BUILD_UI=OFF -DLLAMA_USE_PREBUILT_UI=OFF \
    -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DGGML_LLAMAFILE=OFF -DGGML_BACKEND_DL=OFF \
    -DGGML_CPU_KLEIDIAI=ON -DFETCHCONTENT_SOURCE_DIR_KLEIDIAI="$pilon_kleidiai_source"
cmake --build "$pilon_work/llama-build" --target llama-server --parallel "${PILON_BUILD_JOBS:-4}"
install -m755 "$pilon_work/llama-build/bin/llama-server" "$pilon_output/libpilon_llama.so.new"
"$pilon_ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded "$pilon_output/libpilon_llama.so.new"
mv "$pilon_output/libpilon_llama.so.new" "$pilon_output/libpilon_llama.so"

python3 "$pilon_root/addons/assistant/scripts/android-runtime-smoke.py" --check-elf --runtime "$pilon_output"
(
    cd "$pilon_output"
    sha256sum libpilon_codex.so libpilon_llama.so > SHA256SUMS
)
printf 'Android ARM64 runtime helpers: %s\n' "$pilon_output"
