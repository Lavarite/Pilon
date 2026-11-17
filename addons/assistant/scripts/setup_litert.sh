#!/usr/bin/env bash
set -euo pipefail
pilon_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$pilon_root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
if [[ ! -x "$pilon_root/runtime/litert/bin/python" ]]; then
  uv venv --python 3.12 "$pilon_root/runtime/litert"
fi
uv pip install --link-mode hardlink --python "$pilon_root/runtime/litert/bin/python" \
  --require-hashes -r "$pilon_root/addons/assistant/config/litert-requirements.lock"
"$pilon_root/runtime/litert/bin/python" -c \
  'import litert_lm; litert_lm.set_min_log_severity(litert_lm.LogSeverity.ERROR)'
