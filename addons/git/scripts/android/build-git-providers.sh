#!/usr/bin/env bash
set -euo pipefail

pilon_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$pilon_root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
pilon_work="$pilon_root/build/android/git-providers"
pilon_output="$pilon_root/builds/android/runtime/arm64-v8a"
pilon_licenses="$pilon_root/builds/android/licenses/git-providers"
pilon_ndk="${ANDROID_NDK_ROOT:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}/ndk/27.2.12479018}"
pilon_tools="$pilon_ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
mkdir -p "$pilon_work/downloads" "$pilon_work/keyring-tests" "$pilon_output" "$pilon_licenses"
[[ "$(go env GOVERSION)" == go1.27.3* ]] || { printf 'Go1.27.3 is required for the pinned provider builds.\n' >&2; exit 1; }
export GOTOOLCHAIN=local

download() {
    local pilon_url="$1" pilon_file="$2" pilon_sha="$3"
    if [[ ! -f "$pilon_file" ]]; then
        curl --fail --location --retry 3 --output "$pilon_file.part" "$pilon_url"
        printf '%s  %s\n' "$pilon_sha" "$pilon_file.part" | sha256sum --check
        mv "$pilon_file.part" "$pilon_file"
    fi
    printf '%s  %s\n' "$pilon_sha" "$pilon_file" | sha256sum --check
}

# GitHub source archive pinned by Termux; GitLab's release checksum covers its vendored source archive.
download https://github.com/cli/cli/archive/refs/tags/v2.103.3.tar.gz \
    "$pilon_work/downloads/gh-2.103.3.tar.gz" \
    bb148138a3a2fe023e57c78042fe3b42fc09171c0e8fff4cf983b227b7a2eade
download https://gitlab.com/api/v4/projects/gitlab-org%2Fcli/packages/generic/glab/1%2E128%2E0/glab_1%2E128%2E0_source%2Etar%2Egz \
    "$pilon_work/downloads/glab-1.128.0-source.tar.gz" \
    908296f5aae4d63bb942f6da57910774fb67df79cfe0167caa0d8cb73e7e6a40

cp "$pilon_root/addons/git/platform/android/provider_keyring.go" "$pilon_work/keyring-tests/keyring_pilon.go"
cp "$pilon_root/addons/git/tests/python/provider_keyring_test.go" "$pilon_work/keyring-tests/keyring_pilon_test.go"
printf 'module pilon-keyring-proof\n\ngo 1.27.0\n' > "$pilon_work/keyring-tests/go.mod"
cat > "$pilon_work/keyring-tests/keyring.go" <<'EOF'
package keyring
import "errors"
var ErrNotFound = errors.New("secret not found")
type Keyring interface { Set(string,string,string) error; Get(string,string)(string,error); Delete(string,string)error; DeleteAll(string)error }
var provider Keyring
EOF
(cd "$pilon_work/keyring-tests"; go test -tags pilon_keyring_test -count=1 ./...)

for pilon_provider in gh glab; do
    pilon_source="$pilon_work/$pilon_provider-source"
    pilon_patch="$pilon_root/addons/git/platform/android/provider-$pilon_provider.patch"
    pilon_sha="$(sha256sum "$pilon_patch" "$pilon_root/addons/git/platform/android/provider_keyring.go" | sha256sum | cut -d' ' -f1)"
    if [[ ! -f "$pilon_source/.pilon-source" ]] || [[ "$(cat "$pilon_source/.pilon-source")" != "$pilon_sha" ]]; then
        rm -rf -- "$pilon_source"
        mkdir -p "$pilon_source"
        if [[ "$pilon_provider" == gh ]]; then
            tar -xzf "$pilon_work/downloads/gh-2.103.3.tar.gz" -C "$pilon_source" --strip-components=1
            (cd "$pilon_source"; go mod vendor)
        else
            tar -xzf "$pilon_work/downloads/glab-1.128.0-source.tar.gz" -C "$pilon_source"
        fi
        patch -d "$pilon_source" -p1 < "$pilon_patch"
        python3 - "$pilon_source/vendor/github.com/zalando/go-keyring/keyring_unix.go" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
source = path.read_text()
needle = ' || linux || '
if source.count(needle) != 1: raise ValueError('Unexpected pinned keyring build constraint')
path.write_text(source.replace(needle, ' || (linux && !android) || '))
PY
        cp "$pilon_root/addons/git/platform/android/provider_keyring.go" "$pilon_source/vendor/github.com/zalando/go-keyring/keyring_pilon.go"
        printf '%s\n' "$pilon_sha" > "$pilon_source/.pilon-source"
    fi
    if [[ "$pilon_provider" == gh ]]; then
        pilon_version='-X github.com/cli/cli/v2/internal/build.Version=2.103.3'
    else
        pilon_version='-X main.version=1.128.0'
    fi
    (cd "$pilon_source"
        GOOS=android GOARCH=arm64 CGO_ENABLED=1 \
        CC="$pilon_tools/aarch64-linux-android28-clang" CXX="$pilon_tools/aarch64-linux-android28-clang++" \
        go build -mod=vendor -p="${PILON_BUILD_JOBS:-4}" -trimpath -buildmode=pie \
            -ldflags="-s -w -linkmode external -extldflags '-Wl,-z,max-page-size=16384,-z,common-page-size=16384' $pilon_version" \
            -o "$pilon_output/libpilon_$pilon_provider.so.new" "./cmd/$pilon_provider")
    mv "$pilon_output/libpilon_$pilon_provider.so.new" "$pilon_output/libpilon_$pilon_provider.so"
    mkdir -p "$pilon_source/cmd/pilon-storage-probe"
    cp "$pilon_root/addons/git/tests/android/android_${pilon_provider}_storage_probe.go" "$pilon_source/cmd/pilon-storage-probe/main.go"
    cp "$pilon_root/addons/git/tests/android/android_provider_transport_bridge.go" "$pilon_source/cmd/pilon-storage-probe/bridge.go"
    (cd "$pilon_source"
        GOOS=android GOARCH=arm64 CGO_ENABLED=1 \
        CC="$pilon_tools/aarch64-linux-android28-clang" CXX="$pilon_tools/aarch64-linux-android28-clang++" \
        go build -mod=vendor -p="${PILON_BUILD_JOBS:-4}" -trimpath -buildmode=pie \
            -ldflags="-s -w -linkmode external -extldflags '-Wl,-z,max-page-size=16384,-z,common-page-size=16384'" \
            -o "$pilon_work/$pilon_provider-storage-probe" ./cmd/pilon-storage-probe)
    python3 - "$pilon_source" "$pilon_licenses/$pilon_provider" <<'PY'
from pathlib import Path
import shutil, sys
source, destination = map(Path, sys.argv[1:])
for path in source.rglob('*'):
    if path.is_file() and path.name.upper().startswith(('LICENSE', 'LICENCE', 'COPYING', 'COPYRIGHT', 'NOTICE', 'PATENTS')):
        target = destination / path.relative_to(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
PY
done
download https://raw.githubusercontent.com/golang/go/go1.27.3/LICENSE \
    "$pilon_licenses/Go-LICENSE.txt" \
    911f8f5782931320f5b8d1160a76365b83aea6447ee6c04fa6d5591467db9dad
mkdir -p "$pilon_work/server-proof"
cat > "$pilon_work/server-proof/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.21)
project(PilonProviderProof LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
find_package(Qt6 REQUIRED COMPONENTS Core Network)
add_executable(server "$pilon_root/addons/git/tests/android_provider_keyring_server.cpp")
target_include_directories(server PRIVATE "$pilon_root/addons/git/src")
target_link_libraries(server PRIVATE Qt6::Core Qt6::Network)
EOF
cmake -S "$pilon_work/server-proof" -B "$pilon_work/server-proof/host-build" -GNinja -DCMAKE_BUILD_TYPE=Release
cmake --build "$pilon_work/server-proof/host-build" --parallel "${PILON_BUILD_JOBS:-4}"
printf 'Android Git provider helpers: %s\n' "$pilon_output"
