#!/usr/bin/env bash

set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$project_dir/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
source_dir="$project_dir/src"
build_dir="$PILON_DEV_DIR/builds/work/linux"
output_dir="$PILON_INSTALL_ROOT/.bin"

# Source and scripts are shared; generated files remain local to each device.
# Optional helper setup is opt-in with PILON_BUILD_APP_ONLY=0.
if [[ "${PILON_BUILD_APP_ONLY:-1}" == 0 ]]; then
    bash "$source_dir/scripts/setup_dependencies.sh"
    bash "$source_dir/scripts/setup_documents.sh"
    bash "$project_dir/addons/notes/scripts/setup_handwriting.sh"
    bash "$project_dir/addons/assistant/scripts/build_assistant_runtime.sh"
    bash "$project_dir/addons/assistant/scripts/setup_litert.sh"
    bash "$project_dir/addons/assistant/scripts/build_speech_runtime.sh"
    bash "$source_dir/scripts/build_notes_runtime.sh"
fi

if command -v nproc >/dev/null 2>&1; then
    default_jobs="$(nproc)"
    if (( default_jobs > 8 )); then default_jobs=8; fi
else
    default_jobs="1"
fi

cmake -S "$source_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" \
    -DCMAKE_INSTALL_PREFIX="$output_dir" \
    "$@"
cmake --build "$build_dir" --parallel "${BUILD_JOBS:-$default_jobs}"
cmake --install "$build_dir" --component Pilon
if [[ "${PILON_BUILD_APP_ONLY:-1}" == 0 ]]; then
    python3 "$project_dir/addons/assistant/scripts/setup_agent_workspace.py" --pilon "$output_dir/pilon"
fi
python3 "$project_dir/src/scripts/install-desktop-entry.py" --executable "$output_dir/pilon"

printf 'Built %s\n' "$output_dir/pilon"
