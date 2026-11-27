#!/usr/bin/env bash
set -euo pipefail
pilon_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$pilon_root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
pilon_python="$PILON_RUNTIME_DIR/handwriting/bin/python"
if [[ ! -x "$pilon_python" ]]; then
    uv venv --python 3.12 "$PILON_RUNTIME_DIR/handwriting"
fi
# Runtime wheels are local to each device; never install competing ORT wheels
# into one environment (they all provide the same Python module).
pilon_backend="${PILON_NOTES_OCR_BACKEND:-auto}"
if [[ "$pilon_backend" == auto ]]; then
    pilon_backend=cpu
    for vendor in /sys/class/drm/card*/device/vendor /sys/class/accel/accel*/device/vendor; do
        [[ -r "$vendor" ]] || continue
        case "$(cat "$vendor")" in
            0x8086) [[ "$pilon_backend" == cuda ]] || pilon_backend=openvino ;;
            0x10de) pilon_backend=cuda ;;
        esac
    done
fi
case "$pilon_backend" in
    cpu) pilon_lock=handwriting-requirements.lock ;;
    cuda) pilon_lock=handwriting-cuda-requirements.lock ;;
    openvino) pilon_lock=handwriting-openvino-requirements.lock ;;
    *) printf 'Unknown handwriting backend: %s\n' "$pilon_backend" >&2; exit 1 ;;
esac
printf 'Preparing handwriting runtime: %s\n' "$pilon_backend"
uv pip sync --link-mode hardlink --python "$pilon_python" "$pilon_root/addons/notes/config/$pilon_lock"
