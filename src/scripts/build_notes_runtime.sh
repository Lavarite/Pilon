#!/usr/bin/env bash
set -euo pipefail
# Local office-to-PDF renderer for note imports; no system installation is needed.
pilon_root="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$pilon_root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
pilon_runtime="$PILON_RUNTIME_DIR/libreoffice"
pilon_archive="LibreOffice_26.8.0_Linux_x86-64_deb.tar.gz"
pilon_sha256="d0a6031a3837e48f9854e6d2da6489b9fadbd814afa4741fa32a197741663a22"
if [[ -x "$pilon_runtime/program/soffice" ]]; then exit 0; fi
pilon_stage="$(mktemp -d)"
trap 'rm -rf -- "$pilon_stage"' EXIT
curl -fL --retry 3 "https://download.documentfoundation.org/libreoffice/stable/26.8.0/deb/x86_64/$pilon_archive" -o "$pilon_stage/$pilon_archive"
(cd "$pilon_stage" && echo "$pilon_sha256  $pilon_archive" | sha256sum --check --status)
tar -xzf "$pilon_stage/$pilon_archive" -C "$pilon_stage"
mkdir -p "$pilon_runtime"
for pilon_package in "$pilon_stage"/LibreOffice_*/DEBS/*.deb; do
    pilon_data="$(ar t "$pilon_package" | sed -n '/^data\.tar/p')"
    ar p "$pilon_package" "$pilon_data" | bsdtar -xf - -C "$pilon_runtime"
done
ln -s opt/libreoffice26.8/program "$pilon_runtime/program"
"$pilon_runtime/program/soffice" --headless --version
