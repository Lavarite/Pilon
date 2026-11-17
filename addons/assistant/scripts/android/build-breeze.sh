#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
work="$root/build/android/breeze"
source="$work/source"
commit=f7ac446fb75e594a6913c846bea6c85b637f6ac0
sdk="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
ndk="${ANDROID_NDK_ROOT:-$sdk/ndk/27.2.12479018}"
qt="${PILON_QT_ANDROID:-$HOME/Qt/6.12.0/android_arm64_v8a}"
host_qt="${PILON_QT_HOST:-$HOME/Qt/6.12.0/gcc_64}"
runtime="$root/builds/android/runtime/arm64-v8a"
licenses="$root/builds/android/licenses/breeze"
mkdir -p "$work" "$runtime" "$licenses"
if [[ ! -d "$source/.git" ]]; then
    git clone --depth 1 --branch v0.9.4 --single-branch https://github.com/0xShug0/audio.cpp.git "$source"
fi
[[ $(git -C "$source" rev-parse HEAD) == "$commit" ]]
git -C "$source" diff --quiet
cmake -S "$source" -B "$work/arm64" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
    -DCMAKE_BUILD_TYPE=Release -DAUDIOCPP_VERSION=0.9.4 -DAUDIOCPP_BUILD_C_API=ON \
    -DAUDIOCPP_MODEL_SET=custom -DAUDIOCPP_MODELS=breeze_tts -DAUDIOCPP_DEPLOYMENT_BUILD=ON \
    -DAUDIOCPP_BUILD_NATIVE_MODEL_MANAGER=OFF -DENGINE_ENABLE_OPENMP=OFF -DGGML_OPENMP=OFF \
    -DENGINE_ENABLE_NATIVE_CPU=OFF -DENGINE_ENABLE_LLAMAFILE=OFF -DENGINE_ENABLE_CPU_ALL_VARIANTS=OFF \
    -DENGINE_BUILD_TESTS=OFF -DENGINE_BUILD_EXTENDED_TESTS=OFF -DENGINE_BUILD_MODEL_TESTS=OFF -DENGINE_BUILD_EXAMPLES=OFF
cmake --build "$work/arm64" --target audiocpp -j"${PILON_BUILD_JOBS:-6}"
"$qt/bin/qt-cmake" -S "$root/addons/assistant/platform/android/breeze" -B "$work/worker-arm64" -G Ninja \
    -DANDROID_SDK_ROOT="$sdk" -DANDROID_NDK_ROOT="$ndk" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 \
    -DQT_HOST_PATH="$host_qt" -DCMAKE_BUILD_TYPE=Release \
    -DAUDIOCPP_LIBRARY="$work/arm64/bin/libaudiocpp.so" -DAUDIOCPP_INCLUDE_DIR="$source/include"
cmake --build "$work/worker-arm64" -j"${PILON_BUILD_JOBS:-6}"
for file in "$work/arm64/bin/libaudiocpp.so" "$work/worker-arm64/pilon-breeze"; do
    python3 - "$file" <<'PY'
import subprocess,sys
rows=subprocess.check_output(['readelf','-lW',sys.argv[1]],text=True).splitlines()
loads=[int(row.split()[-1],16) for row in rows if row.strip().startswith('LOAD')]
assert loads and min(loads)>=16384,sys.argv[1]
PY
done
cp "$work/arm64/bin/libaudiocpp.so" "$runtime/libaudiocpp.so.new"
cp "$work/worker-arm64/pilon-breeze" "$runtime/libpilon_breeze.so.new"
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded "$runtime/libaudiocpp.so.new" "$runtime/libpilon_breeze.so.new"
chmod 755 "$runtime/libpilon_breeze.so.new"
mv "$runtime/libaudiocpp.so.new" "$runtime/libaudiocpp.so"
mv "$runtime/libpilon_breeze.so.new" "$runtime/libpilon_breeze.so"
cp "$source/LICENSE" "$licenses/audio.cpp-LICENSE"
cp "$source/external/ggml/LICENSE" "$licenses/ggml-LICENSE"
cp "$source/external/cJSON/LICENSE" "$licenses/cJSON-LICENSE"
cp "$source/external/sentencepiece/LICENSE" "$licenses/sentencepiece-LICENSE"
cp "$source/external/sentencepiece/third_party/protobuf-lite/LICENSE" "$licenses/protobuf-lite-LICENSE"
python3 - "$source/external" "$licenses/external" <<'PY'
from pathlib import Path
import shutil,sys
source,destination=map(Path,sys.argv[1:])
for name in ('ggml','utf8proc','libyaml','sentencepiece','cJSON'):
    for path in (source/name).rglob('*'):
        if path.is_file() and path.name.lower().split('.')[0] in ('license','copying','notice'):
            target=destination/path.relative_to(source)
            target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(path,target)
PY
cp "$root/addons/assistant/platform/android/breeze/llama-tokenizer-LICENSE.txt" "$licenses/llama-tokenizer-LICENSE.txt"
cat > "$licenses/SOURCE.txt" <<EOF
audio.cpp v0.9.4, commit $commit
https://github.com/0xShug0/audio.cpp/tree/$commit
Selective breeze_tts C API, CPU backend, OpenMP/native-host-ISA/llamafile disabled.
Breeze model weights retain their separate upstream license in the model catalog.
Vendored llama_tokenizer attribution: ggml-org/llama.cpp MIT LICENSE,
https://github.com/ggml-org/llama.cpp/blob/4d9176092d00586775af140581bb0b558ddc4389/LICENSE
EOF
printf 'Breeze runtime: %s\n' "$runtime"
