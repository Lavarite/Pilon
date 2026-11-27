#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
set -euo pipefail

if [[ $# != 2 || ! -f $1 ]]; then
    printf 'Usage: %s pilon-VERSION-source.tar.gz AUR_DIRECTORY\n' "$0" >&2
    exit 2
fi

archive=$(realpath -- "$1")
output=$2
name=${1##*/}
if [[ ! $name =~ ^pilon-([0-9]+\.[0-9]+\.[0-9]+)-source\.tar\.gz$ ]]; then
    printf 'Expected a Pilon source release archive.\n' >&2
    exit 2
fi
version=${BASH_REMATCH[1]}
cmake=$(tar -xOf "$archive" "pilon-$version-source/src/CMakeLists.txt")
if ! grep -Fxq "project(Pilon VERSION $version LANGUAGES C CXX)" <<< "$cmake"; then
    printf 'The archive filename and CMake version differ.\n' >&2
    exit 2
fi
checksum=$(sha256sum < "$archive")
checksum=${checksum%% *}

templates=$(cd -- "$(dirname -- "$0")" && pwd)
staging=$(mktemp -d)
trap 'rm -rf -- "$staging"' EXIT
sed -e "s/^pkgver=.*/pkgver=$version/" -e "s/^_source_sha256=.*/_source_sha256=$checksum/" \
    "$templates/PKGBUILD" > "$staging/PKGBUILD"
(cd -- "$staging" && makepkg --printsrcinfo > .SRCINFO)
mkdir -p -- "$output"
install -m644 "$staging/PKGBUILD" "$staging/.SRCINFO" "$templates/LICENSE" "$output/"
