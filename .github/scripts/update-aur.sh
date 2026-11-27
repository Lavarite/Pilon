#!/usr/bin/env bash
set -euo pipefail

recipe=$1
checkout=$2
files=(PKGBUILD .SRCINFO LICENSE)

pkgver() {
    local version
    version=$(awk '$1 == "pkgver" && $2 == "=" { print $3 }' "$1")
    if [[ "$(awk '$1 == "pkgbase" && $2 == "=" { print $3 }' "$1")" != pilon-app ||
          ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        printf 'Expected pilon-app with a valid version in %s.\n' "$1" >&2
        return 1
    fi
    printf '%s\n' "$version"
}

version=$(pkgver "$recipe/.SRCINFO")
comparison=1
if git -C "$checkout" rev-parse --verify --quiet HEAD >/dev/null; then
    current=$(pkgver "$checkout/.SRCINFO")
    comparison=$(vercmp "$version" "$current")
    if (( comparison < 0 )); then
        printf 'AUR already has the newer Pilon %s.\n' "$current"
        exit 0
    fi
fi

for file in "${files[@]}"; do
    install -m644 -- "$recipe/$file" "$checkout/$file"
done
git -C "$checkout" add -- "${files[@]}"
if git -C "$checkout" diff --cached --quiet; then
    printf 'Pilon %s is already on AUR.\n' "$version"
    exit 0
fi
if (( comparison == 0 )); then
    printf 'The recipe changed without a new version. Check the AUR package before replacing it.\n' >&2
    exit 1
fi

git -C "$checkout" -c user.name='Pilon release' -c user.email='41898282+github-actions[bot]@users.noreply.github.com' \
    commit -m "Update Pilon to $version"
git -C "$checkout" push origin HEAD:refs/heads/master
