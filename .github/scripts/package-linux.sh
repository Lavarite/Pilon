#!/usr/bin/env bash
set -euo pipefail

version="${1#v}"
stem="pilon-$version-linux-x86_64"
assets="$PWD/release-assets"
verify="$(mktemp -d)"
trap 'rm -rf -- "$verify"' EXIT

cmake --install build --prefix "$PWD/builds"
python3 src/scripts/package-release.py source --output-dir "$assets"
python3 src/scripts/package-release.py linux --corresponding-source --output-dir "$assets"
python3 - "$assets/$stem-corresponding-source-report.json" <<'PY'
import json
from pathlib import Path
import sys

report = json.loads(Path(sys.argv[1]).read_text())
if report["missing"]:
    raise SystemExit(f"Missing matching source: {report['missing']}")
PY
(
    cd "$assets"
    sha256sum --check "pilon-$version-source-SHA256SUMS" "$stem-SHA256SUMS"
)
tar -xzf "$assets/$stem.tar.gz" -C "$verify"
status=0
(
    cd "$verify/$stem"
    QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software timeout 8s ./builds/pilon
) || status="$?"
if [[ "$status" != 124 ]]; then
    printf 'The extracted executable exited before the smoke test finished (%s).\n' "$status" >&2
    exit 1
fi
sh -n src/scripts/install-linux.sh
install -m 755 src/scripts/install-linux.sh "$assets/install-linux.sh"
