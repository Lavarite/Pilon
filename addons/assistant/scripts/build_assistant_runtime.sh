#!/usr/bin/env bash
set -euo pipefail

pilon_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$pilon_root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
# Transfers through filesystems without Unix modes can strip executable bits.
# Repair only our known runtime entry points before deciding to rebuild.
for pilon_binary in bin/llama-server bin/whisper-cli cuda/bin/llama-server; do
  if [[ -f "$PILON_RUNTIME_DIR/$pilon_binary" && ! -x "$PILON_RUNTIME_DIR/$pilon_binary" ]]; then
    chmod u+x "$PILON_RUNTIME_DIR/$pilon_binary"
  fi
done
pilon_backend="${PILON_LLM_BACKEND:-auto}"
if [[ "$pilon_backend" == auto ]]; then
  pilon_backend=vulkan
  for pilon_vendor in /sys/class/drm/card[0-9]*/device/vendor; do
    [[ -f "$pilon_vendor" ]] || continue
    case "$(cat "$pilon_vendor")" in
      0x10de)
        if command -v nvcc >/dev/null || { [[ -x "$PILON_RUNTIME_DIR/cuda/bin/llama-server" && "${PILON_REBUILD_RUNTIME:-0}" != 1 ]] \
            && cmp -s "$pilon_root/addons/assistant/config/llama-prefix-cache.patch" "$PILON_RUNTIME_DIR/cuda/llama-prefix-cache.patch"; }; then
          pilon_backend=cuda
        else
          echo 'CUDA compiler unavailable; using the Vulkan runtime.' >&2
        fi
        break ;;
    esac
  done
fi
case "$pilon_backend" in
  sycl) bash "$pilon_root/addons/assistant/scripts/install_sycl_runtime.sh" ;;
  cuda|cpu|vulkan) ;;
  *) echo 'PILON_LLM_BACKEND must be auto, cpu, vulkan, sycl or cuda.' >&2; exit 1 ;;
esac
if [[ "${PILON_REBUILD_RUNTIME:-0}" != 1 && -x "$PILON_RUNTIME_DIR/bin/llama-server" \
      && -x "$PILON_RUNTIME_DIR/bin/whisper-cli" ]] \
      && cmp -s "$pilon_root/addons/assistant/config/llama-prefix-cache.patch" "$PILON_RUNTIME_DIR/llama-prefix-cache.patch"; then
  if [[ "$pilon_backend" != cuda ]] || { [[ -x "$PILON_RUNTIME_DIR/cuda/bin/llama-server" ]] \
      && cmp -s "$pilon_root/addons/assistant/config/llama-prefix-cache.patch" "$PILON_RUNTIME_DIR/cuda/llama-prefix-cache.patch"; }; then exit 0; fi
fi
if [[ "$pilon_backend" == cuda ]] && ! command -v nvcc >/dev/null; then
  echo 'CUDA build requires nvcc. Install the CUDA toolkit or use PILON_LLM_BACKEND=vulkan.' >&2
  exit 1
fi
pilon_runtime_work="${PILON_RUNTIME_BUILD_DIR:-$(mktemp -d /tmp/pilon-runtime-build.XXXXXX)}"
mkdir -p "$pilon_runtime_work" "$PILON_RUNTIME_DIR/bin"

curl --fail --location --retry 3 --output "$pilon_runtime_work/llama.tar.gz" \
  https://codeload.github.com/ggml-org/llama.cpp/tar.gz/4d9176092d00586775af140581bb0b558ddc4389
curl --fail --location --retry 3 --output "$pilon_runtime_work/whisper.tar.gz" \
  https://codeload.github.com/ggml-org/whisper.cpp/tar.gz/refs/tags/b4938
curl --fail --location --retry 3 --output "$pilon_runtime_work/spirv-headers.tar.gz" \
  https://codeload.github.com/KhronosGroup/SPIRV-Headers/tar.gz/refs/tags/vulkan-sdk-1.4.341.0
(
  cd "$pilon_runtime_work"
  sha256sum --check <<'CHECKSUMS'
1a76abb8f7fe887c9cd37f71637b1a75334340e4e7fad38ef9b15d1a47ac511a  llama.tar.gz
6d8d70a014ca2b10f8a6d006b8f423e5f5ef2afcfbe92b57ab4e01107238112a  whisper.tar.gz
cab0a654c4917e16367483296b44cdb1d614e3120c721beafcd37e3a8580486c  spirv-headers.tar.gz
CHECKSUMS
)
tar -xzf "$pilon_runtime_work/llama.tar.gz" -C "$pilon_runtime_work"
patch -d "$pilon_runtime_work/llama.cpp-4d9176092d00586775af140581bb0b558ddc4389" -p1 \
  < "$pilon_root/addons/assistant/config/llama-prefix-cache.patch"
tar -xzf "$pilon_runtime_work/whisper.tar.gz" -C "$pilon_runtime_work"
tar -xzf "$pilon_runtime_work/spirv-headers.tar.gz" -C "$pilon_runtime_work"
# Keep Vulkan's build-only headers in the runtime work directory.
cmake -S "$pilon_runtime_work/SPIRV-Headers-vulkan-sdk-1.4.341.0" \
  -B "$pilon_runtime_work/spirv-build" -DCMAKE_INSTALL_PREFIX="$pilon_runtime_work/spirv"
cmake --install "$pilon_runtime_work/spirv-build"
# The pinned ggml checks the SPIRV package but does not link its include target.
export CPLUS_INCLUDE_PATH="$pilon_runtime_work/spirv/include${CPLUS_INCLUDE_PATH:+:$CPLUS_INCLUDE_PATH}"
cmake -S "$pilon_runtime_work/llama.cpp-4d9176092d00586775af140581bb0b558ddc4389" \
  -B "$pilon_runtime_work/llama-build" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TOOLS=ON \
  -DGGML_NATIVE=OFF -DGGML_BACKEND_DL=OFF -DGGML_VULKAN=ON \
  -DCMAKE_PREFIX_PATH="$pilon_runtime_work/spirv"
cmake --build "$pilon_runtime_work/llama-build" --target llama-server -j "${PILON_BUILD_JOBS:-8}"
cmake -S "$pilon_runtime_work/whisper.cpp-b4938" -B "$pilon_runtime_work/whisper-build" \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DWHISPER_BUILD_TESTS=OFF \
  -DGGML_NATIVE=OFF -DGGML_BACKEND_DL=OFF -DGGML_VULKAN=ON \
  -DCMAKE_PREFIX_PATH="$pilon_runtime_work/spirv"
cmake --build "$pilon_runtime_work/whisper-build" --target whisper-cli -j "${PILON_BUILD_JOBS:-8}"
# Rename into place so an open Pilon process can keep using its current executable.
install -m755 "$pilon_runtime_work/llama-build/bin/llama-server" "$PILON_RUNTIME_DIR/bin/llama-server.new"
mv -f "$PILON_RUNTIME_DIR/bin/llama-server.new" "$PILON_RUNTIME_DIR/bin/llama-server"
install -m644 "$pilon_root/addons/assistant/config/llama-prefix-cache.patch" "$PILON_RUNTIME_DIR/llama-prefix-cache.patch"
install -m755 "$pilon_runtime_work/whisper-build/bin/whisper-cli" "$PILON_RUNTIME_DIR/bin/whisper-cli.new"
mv -f "$PILON_RUNTIME_DIR/bin/whisper-cli.new" "$PILON_RUNTIME_DIR/bin/whisper-cli"

if [[ "$pilon_backend" == cuda ]]; then
  cmake -S "$pilon_runtime_work/llama.cpp-4d9176092d00586775af140581bb0b558ddc4389" \
    -B "$pilon_runtime_work/cuda-build" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TOOLS=ON \
    -DGGML_NATIVE=OFF -DGGML_BACKEND_DL=OFF -DGGML_CUDA=ON
  cmake --build "$pilon_runtime_work/cuda-build" --target llama-server -j "${PILON_BUILD_JOBS:-8}"
  mkdir -p "$PILON_RUNTIME_DIR/cuda/bin"
  install -m755 "$pilon_runtime_work/cuda-build/bin/llama-server" "$PILON_RUNTIME_DIR/cuda/bin/llama-server.new"
  mv -f "$PILON_RUNTIME_DIR/cuda/bin/llama-server.new" "$PILON_RUNTIME_DIR/cuda/bin/llama-server"
  install -m644 "$pilon_root/addons/assistant/config/llama-prefix-cache.patch" "$PILON_RUNTIME_DIR/cuda/llama-prefix-cache.patch"
fi
