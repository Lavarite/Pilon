#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
work="$PILON_DEV_DIR/dev-deps/speech"
source="$work/source"
runtime="$PILON_RUNTIME_DIR/speech"
commit=f7ac446fb75e594a6913c846bea6c85b637f6ac0
mkdir -p "$work"
if [[ ! -d "$source/.git" ]]; then
    git clone --depth 1 --branch v0.9.4 --single-branch https://github.com/0xShug0/audio.cpp.git "$source"
fi
[[ $(git -C "$source" rev-parse HEAD) == "$commit" ]]
git -C "$source" diff --quiet
cmake -S "$source" -B "$work/native" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DAUDIOCPP_VERSION=0.9.4 -DAUDIOCPP_BUILD_C_API=ON \
    -DAUDIOCPP_MODEL_SET=custom '-DAUDIOCPP_MODELS=breeze_tts;supertonic;pocket_tts' -DAUDIOCPP_DEPLOYMENT_BUILD=ON \
    -DAUDIOCPP_BUILD_NATIVE_MODEL_MANAGER=OFF -DENGINE_ENABLE_OPENMP=OFF -DGGML_OPENMP=OFF \
    -DENGINE_ENABLE_NATIVE_CPU=OFF -DENGINE_ENABLE_LLAMAFILE=OFF -DENGINE_ENABLE_CPU_ALL_VARIANTS=OFF \
    -DENGINE_BUILD_TESTS=OFF -DENGINE_BUILD_EXTENDED_TESTS=OFF -DENGINE_BUILD_MODEL_TESTS=OFF -DENGINE_BUILD_EXAMPLES=OFF
cmake --build "$work/native" --target audiocpp -j"${PILON_BUILD_JOBS:-6}"
cmake -S "$root/addons/assistant/platform/android/speech" -B "$work/speech-worker" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON -DCMAKE_INSTALL_RPATH='$ORIGIN' \
    -DAUDIOCPP_LIBRARY="$work/native/bin/libaudiocpp.so" -DAUDIOCPP_INCLUDE_DIR="$source/include"
cmake --build "$work/speech-worker" -j"${PILON_BUILD_JOBS:-6}"
install -Dm755 "$work/native/bin/libaudiocpp.so" "$runtime/bin/libaudiocpp.so.0"
install -Dm755 "$work/speech-worker/pilon-speech" "$runtime/bin/pilon-speech"
rm -f -- "$runtime/bin/pilon-breeze"
install -Dm644 "$source/LICENSE" "$runtime/licenses/audio.cpp-LICENSE"
install -Dm644 "$source/external/ggml/LICENSE" "$runtime/licenses/ggml-LICENSE"
install -Dm644 "$source/external/cJSON/LICENSE" "$runtime/licenses/cJSON-LICENSE"
install -Dm644 "$source/external/libyaml/License" "$runtime/licenses/libyaml-LICENSE"
install -Dm644 "$source/external/utf8proc/LICENSE.md" "$runtime/licenses/utf8proc-LICENSE"
install -Dm644 "$source/external/sentencepiece/LICENSE" "$runtime/licenses/sentencepiece-LICENSE"
install -Dm644 "$source/external/sentencepiece/third_party/protobuf-lite/LICENSE" "$runtime/licenses/protobuf-lite-LICENSE"
printf 'Speech runtime: %s\n' "$runtime"
