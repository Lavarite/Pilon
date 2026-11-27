#!/bin/sh
set -eu

releases_page=https://github.com/Lavarite/Pilon/releases
staging=

fail() {
    printf '%s\n' "$*" >&2
    exit 1
}

download() {
    curl -q --fail --silent --show-error --location --retry 3 \
        --proto '=https' --proto-redir '=https' --output "$2" "$1"
}

prepare_release() {
    platform=$1
    extension=$2
    label=$3
    download https://api.github.com/repos/Lavarite/Pilon/releases "$staging/releases.json" ||
        fail "Unable to look up Pilon releases. See $releases_page"
    version=$("$python" - "$staging/releases.json" "$platform" "$extension" <<'PYTHON'
import json
import re
import sys

with open(sys.argv[1], encoding="utf-8") as response:
    releases = json.load(response)
if not isinstance(releases, list):
    sys.exit("Invalid Pilon release list.")
for release in releases:
    tag = re.fullmatch(r"v([0-9]+\.[0-9]+\.[0-9]+)", release["tag_name"])
    if tag and not release["draft"]:
        stem = f"pilon-{tag[1]}-{sys.argv[2]}"
        names = {asset["name"] for asset in release["assets"] if asset["state"] == "uploaded"}
        if {stem + sys.argv[3], stem + "-SHA256SUMS"} <= names:
            print(tag[1])
            break
else:
    sys.exit(1)
PYTHON
) || fail "No published $label package with a checksum found in recent Pilon releases. See $releases_page"
    stem="pilon-$version-$platform"
    asset="$stem$extension"
    checksums="$stem-SHA256SUMS"
    release_url="$releases_page/download/v$version"
    download "$release_url/$checksums" "$staging/$checksums" ||
        fail "Unable to download the SHA-256 checksums. See $releases_page/tag/v$version"
    expected=$(awk -v asset="$asset" '
        $2 == asset || $2 == "*" asset {
            matches++
            if (NF != 2 || length($1) != 64 || $1 ~ /[^0-9a-fA-F]/) exit 1
            print tolower($1)
        }
        END { if (matches != 1) exit 1 }
    ' "$staging/$checksums") || fail "Invalid SHA-256 checksum for $asset. See $releases_page/tag/v$version"
    download "$release_url/$asset" "$staging/$asset" ||
        fail "Unable to download $asset. Open $release_url/$asset in your browser."
    actual=$(cd "$staging" && sha256sum "$asset") || fail "Unable to verify SHA-256 for $asset."
    [ "${actual%% *}" = "$expected" ] || fail "SHA-256 verification failed for $asset."
}

check_executable() {
    [ -x "$1" ] || fail "Pilon executable missing at $1"
    if ! libraries=$(ldd "$1" 2>&1) || printf '%s\n' "$libraries" | grep -q 'not found'; then
        printf '%s\n' "$libraries" >&2
        fail 'Install compatible Linux runtime libraries. See https://github.com/Lavarite/Pilon/blob/main/docs/install.md#linux'
    fi
}

install_linux() {
    [ "$(uname -s)/$(uname -m)" = Linux/x86_64 ] || fail 'Pilon requires Linux x86_64 for this installer.'
    [ "$(id -u)" -ne 0 ] || fail 'Run this installer as your normal user.'
    for command in curl python3 sha256sum tar ldd; do
        command -v "$command" >/dev/null 2>&1 || fail "Missing required command: $command"
    done

    prefix="${PILON_INSTALL_ROOT:-$HOME/Pilon}"
    mkdir -p "$prefix"
    staging=$(mktemp -d "$prefix/.install.XXXXXXXX")
    python=python3
    prepare_release linux-x86_64 .tar.gz 'Linux x86_64'
    tar -xzf "$staging/$asset" -C "$staging"
    bundle="$staging/$stem"
    binaries="$bundle/.bin"
    destination="$prefix/.bin"
    if [ ! -d "$binaries" ]; then
        binaries="$bundle/bin"
        [ -d "$binaries" ] || binaries="$bundle/builds"
        destination="$prefix/bin"
    fi
    check_executable "$binaries/pilon"
    [ ! -L "$destination" ] || fail 'The installation bin folder cannot be a symbolic link.'
    [ ! -L "$destination/addons" ] || fail 'The add-on staging folder cannot be a symbolic link.'
    mkdir -p "$destination/addons"
    for target in "$destination/pilon" "$destination/addons/.install-packages"; do
        [ ! -L "$target" ] && { [ ! -e "$target" ] || [ -f "$target" ]; } || fail "Cannot replace $target"
    done
    for package in "$binaries/addons/"*.piext; do
        target="$destination/addons/${package##*/}"
        [ -f "$package" ] && [ ! -L "$package" ] || fail "Invalid package $package"
        [ ! -L "$target" ] && { [ ! -e "$target" ] || [ -f "$target" ]; } || fail "Cannot replace $target"
    done
    for package in "$binaries/addons/"*.piext; do
        mv -T -- "$package" "$destination/addons/${package##*/}"
    done
    [ ! -f "$binaries/addons/.install-packages" ] ||
        mv -T -- "$binaries/addons/.install-packages" "$destination/addons/.install-packages"
    mv -T -- "$binaries/pilon" "$destination/pilon"
    python3 "$bundle/src/scripts/install-desktop-entry.py" --executable "$destination/pilon"
    printf 'Installed Pilon %s. Open Pilon from your application menu.\n' "$version"
}

install_android() {
    case "$android_sdk" in
        *[!0-9]*) fail "Unable to determine the Android version. See $releases_page" ;;
    esac
    [ "$android_sdk" -ge 28 ] || fail 'Pilon requires Android 9 (API 28) or later.'
    case ",$("$android_getprop" ro.product.cpu.abilist)," in
        *,arm64-v8a,*) ;;
        *) fail 'Pilon requires Android ARM64 (arm64-v8a).' ;;
    esac
    for command in curl python3 sha256sum awk; do
        command -v "$command" >/dev/null 2>&1 || fail "Missing required command: $command. Download the Android APK from $releases_page"
    done
    [ -n "${HOME:-}" ] || fail "No home directory for the APK. Download it from $releases_page"
    downloads="$HOME/Downloads"
    if [ -d "$HOME/storage/downloads" ] && [ -w "$HOME/storage/downloads" ]; then
        downloads="$HOME/storage/downloads"
    fi
    mkdir -p "$downloads" && staging=$(mktemp -d "$downloads/Pilon.XXXXXXXX") ||
        fail "Unable to save the APK in $downloads. Download it from $releases_page"
    python=python3
    prepare_release android-arm64-v8a .apk 'Android ARM64'
    apk="$staging/$asset"
    rm -- "$staging/releases.json" "$staging/$checksums"
    # Android may open the file after termux-open exits.
    staging=
    printf 'Verified APK saved to %s\n' "$apk"
    if command -v termux-open >/dev/null 2>&1 &&
        termux-open --view --content-type application/vnd.android.package-archive "$apk"; then
        printf 'Follow the Android package installer prompts if they open.\n'
    else
        printf 'Open the saved APK with your file manager.\n'
    fi
    printf 'APK download for your browser: %s/%s\n' "$release_url" "$asset"
}

trap 'if [ -n "$staging" ]; then rm -rf -- "$staging"; fi' 0
trap 'exit 1' HUP INT TERM

android_sdk=
if [ "$(uname -s)" = Linux ]; then
    android_getprop=
    if command -v getprop >/dev/null 2>&1; then
        android_getprop=getprop
    elif [ -x /system/bin/getprop ]; then
        android_getprop=/system/bin/getprop
    fi
    if [ -n "$android_getprop" ]; then
        android_sdk=$("$android_getprop" ro.build.version.sdk 2>/dev/null || :)
    fi
fi
if [ -n "$android_sdk" ]; then
    install_android
else
    install_linux
fi
