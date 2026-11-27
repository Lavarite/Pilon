#!/usr/bin/env bash
set -euo pipefail

# The native worker and Intel libraries are isolated from Qt and system drivers.
pilon_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$pilon_root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
pilon_sycl="$PILON_RUNTIME_DIR/sycl"
pilon_tag=b10964
pilon_archive_sha=31699a7742ee6a3177be642baeecdc38cecf593ac74870e722cea0dbe8c291c9
if [[ "$(uname -m)" != x86_64 ]]; then
  echo 'The packaged SYCL worker requires Linux x86_64.' >&2
  exit 1
fi
mkdir -p "$pilon_sycl/bin"
if [[ ! -x "$pilon_sycl/oneapi/bin/python" ]]; then
  uv venv --python 3.12 "$pilon_sycl/oneapi"
fi
uv pip install --python "$pilon_sycl/oneapi/bin/python" \
  'dpcpp-cpp-rt==2025.3.3' 'onednn==2025.3.0' 'onemkl-sycl-blas==2025.3.1'
uv pip freeze --python "$pilon_sycl/oneapi/bin/python" > "$pilon_sycl/packages.txt"

if [[ ! -x "$pilon_sycl/native/llama-server" || ! -f "$pilon_sycl/release.txt" \
      || "$(cat "$pilon_sycl/release.txt")" != "$pilon_archive_sha" ]]; then
  pilon_stage="$(mktemp -d "$pilon_sycl/.install.XXXXXX")"
  trap 'rm -rf -- "$pilon_stage"' EXIT
  curl --fail --location --retry 3 \
    "https://github.com/ggml-org/llama.cpp/releases/download/$pilon_tag/llama-$pilon_tag-bin-ubuntu-sycl-fp32-x64.tar.gz" \
    --output "$pilon_stage/runtime.tar.gz"
  printf '%s  %s\n' "$pilon_archive_sha" "$pilon_stage/runtime.tar.gz" | sha256sum --check
  mkdir "$pilon_stage/native"
  tar -xzf "$pilon_stage/runtime.tar.gz" --strip-components=1 -C "$pilon_stage/native"
  # Install new files atomically so a running worker can retain its mapped files.
  mkdir -p "$pilon_sycl/native"
  for pilon_file in "$pilon_stage/native/"*; do
    mv -f -- "$pilon_file" "$pilon_sycl/native/"
  done
  printf '%s\n' "$pilon_archive_sha" > "$pilon_sycl/release.txt"
fi

cat > "$pilon_sycl/bin/llama-server.new" <<'WRAPPER'
#!/usr/bin/env bash
set -euo pipefail
pilon_sycl="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
export LD_LIBRARY_PATH="$pilon_sycl/native:$pilon_sycl/oneapi/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# oneAPI 2025.3.3 can crash in getSortedImages when its persistent code cache
# handles this worker's kernels. In-process and Intel driver caching still work.
export SYCL_CACHE_PERSISTENT="${SYCL_CACHE_PERSISTENT:-0}"
exec "$pilon_sycl/native/llama-server" "$@"
WRAPPER
chmod 755 "$pilon_sycl/bin/llama-server.new"
mv -f "$pilon_sycl/bin/llama-server.new" "$pilon_sycl/bin/llama-server"
"$pilon_sycl/bin/llama-server" --list-devices
