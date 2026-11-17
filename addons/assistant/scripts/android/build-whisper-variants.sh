#!/usr/bin/env bash
set -euo pipefail
pilon_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$pilon_root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
pilon_source="$pilon_root/build/android/media-runtime/whisper.cpp-b4938"
pilon_build="$pilon_root/build/android/media-runtime/whisper-variants-build"
pilon_kleidiai="${PILON_WHISPER_KLEIDIAI:-OFF}"
if [[ "$pilon_kleidiai" == ON ]]; then pilon_build="$pilon_root/build/android/media-runtime/whisper-kleidiai-variants-build"; fi
pilon_kleidiai_source="$pilon_root/build/android/media-runtime/kleidiai-v1.24.0"
if [[ "$pilon_kleidiai" == ON ]]; then
    pilon_archive="$pilon_root/build/android/media-runtime/downloads/kleidiai-v1.24.0-src.tar.gz"
    if [[ ! -f "$pilon_archive" ]]; then pilon_archive="$pilon_root/build/android/runtime/downloads/kleidiai-v1.24.0-src.tar.gz"; fi
    printf '%s  %s\n' 9348b969e042d8890a54b01a463dbe71f5a4c074b5329e9c26a85ef3b68aa19b "$pilon_archive" | sha256sum --check
    if [[ ! -f "$pilon_kleidiai_source/CMakeLists.txt" ]]; then tar -xzf "$pilon_archive" -C "$pilon_root/build/android/media-runtime"; fi
fi
pilon_ndk="${ANDROID_NDK_ROOT:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}/ndk/27.2.12479018}"
pilon_linker_flags="-Wl,-z,max-page-size=16384,-z,common-page-size=16384 -Wl,-rpath,'\$ORIGIN'"
[[ -f "$pilon_source/CMakeLists.txt" ]] || { printf 'Run the pinned media build first.\n' >&2; exit 1; }
cmake -S "$pilon_source" -B "$pilon_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$pilon_ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_shared \
    -DCMAKE_EXE_LINKER_FLAGS="$pilon_linker_flags" \
    -DCMAKE_SHARED_LINKER_FLAGS="$pilon_linker_flags" \
    -DCMAKE_MODULE_LINKER_FLAGS="$pilon_linker_flags" \
    -DBUILD_SHARED_LIBS=ON -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_EXAMPLES=ON \
    -DGGML_NATIVE=OFF -DGGML_BACKEND_DL=ON -DGGML_CPU_ALL_VARIANTS=ON -DGGML_OPENMP=OFF \
    -DGGML_CPU_KLEIDIAI="$pilon_kleidiai" \
    -DFETCHCONTENT_SOURCE_DIR_KLEIDIAI_DOWNLOAD="$pilon_kleidiai_source"
cmake --build "$pilon_build" --target whisper-cli --parallel "${PILON_BUILD_JOBS:-6}"
python3 - "$pilon_root" "$pilon_build/bin" <<'PY'
import importlib.util, sys
from pathlib import Path
spec = importlib.util.spec_from_file_location('runtime_check', sys.argv[1]+'/addons/assistant/scripts/android-runtime-smoke.py')
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
for path in Path(sys.argv[2]).iterdir():
    if path.name == 'whisper-cli' or path.suffix == '.so':
        with path.open('rb') as source: module.check_alignment(source,path.name)
PY
printf 'Isolated Whisper variants ready: %s/bin\n' "$pilon_build"
