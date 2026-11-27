#!/usr/bin/env bash
set -euo pipefail

script=$(cd -- "$(dirname -- "$0")" && pwd)/update-aur.sh
stage=$(mktemp -d)
trap 'rm -rf -- "$stage"' EXIT
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null
git init --quiet --bare --initial-branch=master "$stage/remote"
mkdir "$stage/recipe"
printf 'Packaged license\n' > "$stage/recipe/LICENSE"

publish() {
    local version=$1
    printf 'pkgname=pilon-app\npkgver=%s\n' "$version" > "$stage/recipe/PKGBUILD"
    printf 'pkgbase = pilon-app\n\tpkgver = %s\n' "$version" > "$stage/recipe/.SRCINFO"
    git -c init.defaultBranch=master clone --quiet "$stage/remote" "$stage/checkout"
    bash "$script" "$stage/recipe" "$stage/checkout"
}

publish 1.0.0
first=$(git -C "$stage/remote" rev-parse HEAD)
rm -rf -- "$stage/checkout"
publish 1.0.0
test "$(git -C "$stage/remote" rev-parse HEAD)" = "$first"
rm -rf -- "$stage/checkout"
publish 1.1.0
second=$(git -C "$stage/remote" rev-parse HEAD)
test "$second" != "$first"
rm -rf -- "$stage/checkout"
publish 1.0.0
test "$(git -C "$stage/remote" rev-parse HEAD)" = "$second"

printf 'pkgbase = another-package\n\tpkgver = 1.2.0\n' > "$stage/recipe/.SRCINFO"
if bash "$script" "$stage/recipe" "$stage/checkout"; then
    printf 'The updater accepted a different package name.\n' >&2
    exit 1
fi
test "$(git -C "$stage/remote" rev-parse HEAD)" = "$second"
rm -rf -- "$stage/checkout"
printf 'Different license content\n' > "$stage/recipe/LICENSE"
if publish 1.1.0; then
    printf 'A rerun replaced the package without a new version.\n' >&2
    exit 1
fi
test "$(git -C "$stage/remote" rev-parse HEAD)" = "$second"
