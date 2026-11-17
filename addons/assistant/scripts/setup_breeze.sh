#!/usr/bin/env bash
set -euo pipefail
pilon_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$pilon_root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
pilon_commit=43e2ea1595297c4059477e2e4a300653761c759b
if [[ ! -f "$pilon_root/addons/assistant/vendor/breeze/models/generation_breeze.py" ]]; then
pilon_work="$(mktemp -d /tmp/pilon-breeze.XXXXXX)"
curl --fail --location --retry 3 -o "$pilon_work/breeze.tar.gz" \
  "https://codeload.github.com/breezeblue-ai/breeze-tts/tar.gz/$pilon_commit"
(cd "$pilon_work"; sha256sum --check <<'CHECKSUM'
994c456fd22e2ff5d6d3125ff52ad8ac55abfa22983c4f9d21cda45ce277bf4f  breeze.tar.gz
CHECKSUM
)
mkdir -p "$pilon_root/addons/assistant/vendor/breeze"
tar -xzf "$pilon_work/breeze.tar.gz" --strip-components=1 -C "$pilon_root/addons/assistant/vendor/breeze"
fi
if [[ ! -x "$pilon_root/runtime/breeze/bin/python" ]]; then
  uv venv --python 3.12 "$pilon_root/runtime/breeze"
fi
# CPU machines do not need the CUDA libraries shipped by the default Torch wheel.
pilon_torch_backend="${PILON_TORCH_BACKEND:-auto}"
if [[ "$pilon_torch_backend" == auto ]]; then
  pilon_torch_backend=cpu
  for pilon_vendor in /sys/class/drm/card*/device/vendor; do
    if [[ -r "$pilon_vendor" && "$(cat "$pilon_vendor")" == 0x10de ]]; then
      pilon_torch_backend=cu128; break
    fi
  done
fi
pilon_lock="$(mktemp /tmp/pilon-breeze-requirements.XXXXXX)"
trap 'rm -f "$pilon_lock"' EXIT
# Local-version pins ensure switching backend replaces an already installed wheel.
sed -E "s/^(torch|torchaudio)==([0-9.]+)$/\\1==\\2+$pilon_torch_backend/" \
  "$pilon_root/addons/assistant/config/breeze-requirements.lock" > "$pilon_lock"
if [[ "$pilon_torch_backend" == cpu ]]; then
  sed -i -E '/^(nvidia-|triton==)/d' "$pilon_lock"
fi
uv pip sync --python "$pilon_root/runtime/breeze/bin/python" \
  --torch-backend "$pilon_torch_backend" --link-mode hardlink "$pilon_lock"
