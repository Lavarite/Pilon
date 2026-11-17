#!/bin/sh
set -eu

fail() {
    printf '%s\n' "$*" >&2
    exit 1
}

download() {
    curl -q --fail --silent --show-error --location --retry 3 \
        --proto '=https' --proto-redir '=https' --output "$2" "$1"
}

check_executable() {
    [ -x "$1" ] || fail "Pilon executable missing at $1"
    if ! libraries=$(ldd "$1" 2>&1) || printf '%s\n' "$libraries" | grep -q 'not found'; then
        printf '%s\n' "$libraries" >&2
        fail 'Install compatible Linux runtime libraries. See https://github.com/Lavarite/Pilon/blob/main/docs/install.md#linux'
    fi
}

install_pilon() {
    [ "$(uname -s)/$(uname -m)" = Linux/x86_64 ] || fail 'Pilon requires Linux x86_64 for this installer.'
    [ "$(id -u)" -ne 0 ] || fail 'Run this installer as your normal user.'
    for command in curl python3 sha256sum tar ldd; do
        command -v "$command" >/dev/null 2>&1 || fail "Missing required command: $command"
    done

    prefix="$HOME/.local/opt/pilon"
    mkdir -p "$prefix"
    staging=$(mktemp -d "$prefix/.install.XXXXXXXX")
    trap 'rm -rf -- "$staging"' EXIT
    trap 'exit 1' HUP INT TERM

    download https://api.github.com/repos/Lavarite/Pilon/releases "$staging/releases.json"
    version=$(python3 - "$staging/releases.json" <<'PYTHON'
import json
import re
import sys

with open(sys.argv[1]) as response:
    for release in json.load(response):
        tag = re.fullmatch(r"v(\d+\.\d+\.\d+)", release["tag_name"])
        if tag and not release["draft"]:
            stem = f"pilon-{tag[1]}-linux-x86_64"
            names = {asset["name"] for asset in release["assets"] if asset["state"] == "uploaded"}
            if {stem + ".tar.gz", stem + "-SHA256SUMS"} <= names:
                print(tag[1])
                break
    else:
        sys.exit("No published Linux x86_64 package found in recent Pilon releases.")
PYTHON
)
    stem="pilon-$version-linux-x86_64"
    destination="$prefix/$stem"
    if [ ! -d "$destination" ]; then
        release="https://github.com/Lavarite/Pilon/releases/download/v$version"
        download "$release/$stem.tar.gz" "$staging/$stem.tar.gz"
        download "$release/$stem-SHA256SUMS" "$staging/$stem-SHA256SUMS"
        (
            cd "$staging"
            sha256sum --check --ignore-missing --strict "$stem-SHA256SUMS"
        )
        tar -xzf "$staging/$stem.tar.gz" -C "$staging"
        check_executable "$staging/$stem/builds/pilon"
        mv -T --no-clobber "$staging/$stem" "$destination"
    fi
    check_executable "$destination/builds/pilon"
    python3 "$destination/src/scripts/install-desktop-entry.py"
    printf 'Installed Pilon %s. Open Pilon from your application menu.\n' "$version"
}

install_pilon
