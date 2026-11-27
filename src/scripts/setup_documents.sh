#!/usr/bin/env bash
set -euo pipefail
pilon_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$pilon_root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
pilon_python="$PILON_RUNTIME_DIR/documents/bin/python"
if [[ ! -x "$pilon_python" ]]; then
    uv venv --python 3.12 "$PILON_RUNTIME_DIR/documents"
fi
uv pip sync --python "$pilon_python" "$pilon_root/src/core/documents/config/document-requirements.lock"
