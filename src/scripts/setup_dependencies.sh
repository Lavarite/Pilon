#!/usr/bin/env bash
set -euo pipefail
pilon_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$pilon_root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
# Both development devices run Arch. Install only this project's prerequisites.
if ! command -v pacman >/dev/null 2>&1; then
    printf 'Install C++23, CMake, Qt 6.10+ (Quick, Multimedia, SVG), libssh2, curl, uv, ffmpeg, poppler, Vulkan headers/loader, shaderc, tar and libarchive, then run PILON_BUILD_APP_ONLY=1 ./src/scripts/build.sh after the setup scripts. Automatic dependency installation currently supports Arch Linux.\n' >&2
    exit 1
fi
packages=(gcc make cmake pkgconf qt6-base qt6-declarative qt6-multimedia qt6-svg
    libical libsecret libssh2 curl ffmpeg poppler vulkan-headers vulkan-icd-loader shaderc uv python tar binutils libarchive git github-cli)
# Intel GPU/NPU inference needs userspace compute drivers in addition to the
# kernel display driver. Install them only on devices that expose this hardware.
for vendor in /sys/class/drm/card*/device/vendor; do
    if [[ -r "$vendor" && "$(cat "$vendor")" == 0x8086 ]]; then
        packages+=(intel-compute-runtime level-zero-loader); break
    fi
done
for vendor in /sys/class/accel/accel*/device/vendor; do
    if [[ -r "$vendor" && "$(cat "$vendor")" == 0x8086 ]]; then
        packages+=(intel-npu-driver intel-npu-compiler); break
    fi
done
missing=()
for package in "${packages[@]}"; do
    if ! pacman -Q "$package" >/dev/null 2>&1; then missing+=("$package"); fi
done
if (( ${#missing[@]} )); then
    if (( EUID == 0 )); then pacman -S --needed "${missing[@]}"
    else sudo pacman -S --needed "${missing[@]}"; fi
fi
