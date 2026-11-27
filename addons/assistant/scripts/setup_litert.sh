#!/usr/bin/env bash
set -euo pipefail
pilon_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$pilon_root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
if [[ ! -x "$PILON_RUNTIME_DIR/litert/bin/python" ]]; then
  uv venv --python 3.12 "$PILON_RUNTIME_DIR/litert"
fi
uv pip install --link-mode hardlink --python "$PILON_RUNTIME_DIR/litert/bin/python" \
  --require-hashes -r "$pilon_root/addons/assistant/config/litert-requirements.lock"
"$PILON_RUNTIME_DIR/litert/bin/python" -c \
  'import litert_lm; litert_lm.set_min_log_severity(litert_lm.LogSeverity.ERROR)'
install -Dm644 "$pilon_root/addons/assistant/workers/litert_worker.py" "$PILON_RUNTIME_DIR/litert/workers/litert_worker.py"
