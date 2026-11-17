#!/usr/bin/env bash
set -euo pipefail

pilon_test_dns=0
if [[ "${1:-}" == "--test-dns" && "$#" == 1 ]]; then
    pilon_test_dns=1
elif [[ "$#" != 0 ]]; then
    printf 'Usage: %s [--test-dns]\n' "$0" >&2; exit 2
fi

pilon_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$pilon_root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
pilon_work="$pilon_root/build/android/codex"
pilon_source="$pilon_work/source/codex-rust-v0.167.0"
pilon_output="$pilon_root/builds/android/runtime/arm64-v8a/libpilon_codex.so"
pilon_dns_output="$pilon_root/builds/android/runtime/arm64-v8a/libpilon_dns.so"
pilon_licenses="$pilon_root/builds/android/licenses/runtime/codex"
pilon_ndk="${ANDROID_NDK_ROOT:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}/ndk/27.2.12479018}"
pilon_patch="$pilon_root/addons/assistant/platform/android/codex-shell.patch"
pilon_dns_source="$pilon_root/addons/assistant/platform/android/codex-dns.c"
pilon_dns_tests="$pilon_root/addons/assistant/platform/android/tests/test-codex-dns.py"
pilon_input="$(sha256sum "$pilon_patch" "$pilon_dns_source" "$pilon_dns_tests" "${BASH_SOURCE[0]}" | sha256sum | cut -d' ' -f1)"
mkdir -p "$pilon_work/downloads" "$pilon_work/tools" "$pilon_licenses" "$(dirname "$pilon_output")"
if [[ "$pilon_test_dns" == 0 && -f "$pilon_work/verified-build" && -f "$pilon_output" && -f "$pilon_dns_output" && -f "$pilon_licenses/SOURCE.txt" ]] \
    && [[ "$(head -n1 "$pilon_work/verified-build")" == "$pilon_input" ]] \
    && [[ "$(sha256sum "$pilon_output" | cut -d' ' -f1)" == "$(sed -n '2p' "$pilon_work/verified-build")" ]] \
    && [[ "$(sha256sum "$pilon_dns_output" | cut -d' ' -f1)" == "$(sed -n '3p' "$pilon_work/verified-build")" ]]; then
    printf 'Android Codex source build is current.\n'; exit 0
fi

download() {
    local url="$1" file="$pilon_work/downloads/$2" digest="$3"
    if [[ ! -f "$file" ]]; then
        curl --fail --location --retry 3 --output "$file.part" "$url"
        printf '%s  %s\n' "$digest" "$file.part" | sha256sum --check
        mv "$file.part" "$file"
    fi
    printf '%s  %s\n' "$digest" "$file" | sha256sum --check
}
download https://codeload.github.com/openai/codex/tar.gz/refs/tags/rust-v0.167.0 codex.tar.gz \
    313e49728fba6c3d44683554c28018f36781f9f9b6bd09eaa65dccdd6c13b1a7
download https://static.rust-lang.org/rustup/archive/1.28.2/x86_64-unknown-linux-gnu/rustup-init rustup-init \
    20a06e644b0d9bd2fbdbfd52d42540bdde820ea7df86e92e533c073da0cdd43c
download https://ziglang.org/download/0.14.1/zig-x86_64-linux-0.14.1.tar.xz zig.tar.xz \
    24aeeec8af16c381934a6cd7d95c807a8cb2cf7df9fa40d359aa884195c4716c
export RUSTUP_HOME="$pilon_work/rustup" CARGO_HOME="$pilon_work/cargo"
export CARGO_TARGET_DIR="$pilon_work/target"
export STABLE_GIT_COMMIT=086ccc833bc9cf01024ad2dd6af7dbd421b6bf9a
if [[ ! -f "$RUSTUP_HOME/toolchains/1.95.0-x86_64-unknown-linux-gnu/bin/rustc" ]]; then
    chmod 755 "$pilon_work/downloads/rustup-init"
    "$pilon_work/downloads/rustup-init" -y --no-modify-path --profile minimal \
        --default-toolchain 1.95.0 --target aarch64-unknown-linux-musl
fi
export PATH="$CARGO_HOME/bin:$PATH"
if [[ ! -f "$pilon_work/tools/zig-x86_64-linux-0.14.1/zig" ]]; then
    tar -xf "$pilon_work/downloads/zig.tar.xz" -C "$pilon_work/tools"
fi
if [[ ! -f "$pilon_source/.pilon-shell-patch" ]] || ! cmp -s "$pilon_patch" "$pilon_source/.pilon-shell-patch"; then
    rm -rf -- "$pilon_source"
    mkdir -p "$pilon_work/source"
    tar -xzf "$pilon_work/downloads/codex.tar.gz" -C "$pilon_work/source"
    patch -d "$pilon_source" -p1 < "$pilon_patch"
    cp "$pilon_patch" "$pilon_source/.pilon-shell-patch"
fi

cc -std=c17 -O2 -Wall -Wextra -Werror "$pilon_dns_source" -o "$pilon_work/dns-host-test"
python3 "$pilon_dns_tests" "$pilon_work/dns-host-test"
if [[ "$pilon_test_dns" == 1 ]]; then
    cargo +1.95.0 test --offline --manifest-path "$pilon_source/codex-rs/Cargo.toml" \
        -p codex-http-client -p codex-websocket-client -j"${PILON_BUILD_JOBS:-4}" platform_dns
    exit 0
fi
"$pilon_ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang" \
    -std=c17 -O2 -Wall -Wextra -Werror -Wl,-z,max-page-size=16384 \
    "$pilon_dns_source" -o "$pilon_dns_output.new"
"$pilon_ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded "$pilon_dns_output.new"

python3 - "$pilon_work" <<'PY'
from pathlib import Path
import sys, textwrap
w = Path(sys.argv[1]); zig = str(w/'tools/zig-x86_64-linux-0.14.1/zig')
for name, language in [('zigcc', 'cc'), ('zigcxx', 'c++')]:
    path = w/name
    path.write_text('#!/usr/bin/env python3\nimport os,sys\nargs=[];skip=False\n'
        'for x in sys.argv[1:]:\n'
        ' if skip:skip=False;continue\n'
        ' if x in ("--target","-target"):skip=True;continue\n'
        ' if x.startswith(("--target=","-target=")):continue\n'
        ' args.append(x)\n'
        f'os.execv({zig!r},[{zig!r},{language!r},"-target","aarch64-linux-musl","-fno-sanitize=undefined"]+args)\n')
    path.chmod(0o755)
path = w/'rust-linker'
linker = str(w/'rustup/toolchains/1.95.0-x86_64-unknown-linux-gnu/lib/rustlib/x86_64-unknown-linux-gnu/bin/rust-lld')
path.write_text('#!/usr/bin/env python3\nimport os,sys\nargs=[]\n'
    'for x in sys.argv[1:]:\n'
    ' if x in ("-nostartfiles","-nodefaultlibs","-nostdlib"):continue\n'
    ' if x.startswith("-Wl,"):args.extend(x[4:].split(","))\n'
    ' else:args.append(x)\n'
    f'os.execv({linker!r},[{linker!r},"-flavor","gnu"]+args)\n')
path.chmod(0o755)
source = w/'source/codex-rust-v0.167.0'
action = (source/'.github/actions/setup-rusty-v8/action.yml').read_text()
assert action.count('run: |') == 1
(w/'setup-v8.sh').write_text(textwrap.dedent(action.split('run: |',1)[1]).lstrip())
PY
export TARGET=aarch64-unknown-linux-musl RUNNER_TEMP="$pilon_work" GITHUB_WORKSPACE="$pilon_source"
export GITHUB_ENV="$pilon_work/v8.env"
pilon_v8="$pilon_work/rusty_v8"
if ! (cd "$pilon_v8" && sha256sum --check rusty_v8_ptrcomp_sandbox_release_aarch64-unknown-linux-musl.sha256) 2>/dev/null; then
    : > "$GITHUB_ENV"
    bash "$pilon_work/setup-v8.sh"
fi
export RUSTY_V8_ARCHIVE="$pilon_v8/librusty_v8_ptrcomp_sandbox_release_aarch64-unknown-linux-musl.a.gz"
export RUSTY_V8_SRC_BINDING_PATH="$pilon_v8/src_binding_ptrcomp_sandbox_release_aarch64-unknown-linux-musl.rs"
export GITHUB_ENV="$pilon_work/openssl.env" OPENSSL_CC="$pilon_work/zigcc" OPENSSL_BUILD_JOBS="${PILON_BUILD_JOBS:-8}"
: > "$GITHUB_ENV"
bash "$pilon_source/.github/scripts/install-musl-openssl.sh"
set -a; source "$GITHUB_ENV"; set +a
export CC_aarch64_unknown_linux_musl="$pilon_work/zigcc"
export CXX_aarch64_unknown_linux_musl="$pilon_work/zigcxx"
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_MUSL_LINKER="$pilon_work/rust-linker"
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_MUSL_RUSTFLAGS="-C link-arg=-Wl,-z,max-page-size=16384 -C link-arg=-Wl,-z,common-page-size=16384"
export AWS_LC_SYS_NO_JITTER_ENTROPY_aarch64_unknown_linux_musl=1
export CXXFLAGS_aarch64_unknown_linux_musl="-pthread -Wno-error=frame-larger-than"
# Upstream release tags bump workspace versions without rewriting Cargo.lock.
cargo +1.95.0 update --workspace --manifest-path "$pilon_source/codex-rs/Cargo.toml"
python3 - "$pilon_work" <<'PY'
from pathlib import Path
import sys, tarfile, tomllib
w = Path(sys.argv[1])
with tarfile.open(w/'downloads/codex.tar.gz') as archive:
    original = tomllib.loads(archive.extractfile('codex-rust-v0.167.0/codex-rs/Cargo.lock').read().decode())
current = tomllib.loads((w/'source/codex-rust-v0.167.0/codex-rs/Cargo.lock').read_text())
def external(lock):
    return sorted((p['name'],p['version'],p['source'],p.get('checksum','')) for p in lock['package'] if p.get('source'))
assert external(original) == external(current), 'The pinned third-party dependency graph changed'
PY
cargo +1.95.0 build --locked --manifest-path "$pilon_source/codex-rs/Cargo.toml" \
    --target "$TARGET" --release -p codex-cli --bin codex -j"${PILON_BUILD_JOBS:-8}"
install -m755 "$CARGO_TARGET_DIR/$TARGET/release/codex" "$pilon_output.new"
"$pilon_ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded "$pilon_output.new"
python3 - "$pilon_root" "$pilon_output.new" <<'PY'
import importlib.util, subprocess, sys
spec = importlib.util.spec_from_file_location('runtime_check', sys.argv[1]+'/addons/assistant/scripts/android-runtime-smoke.py')
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
with open(sys.argv[2], 'rb') as source: module.check_alignment(source, 'Codex')
assert '(NEEDED)' not in subprocess.check_output(['readelf','-d',sys.argv[2]],text=True)
assert 'INTERP' not in subprocess.check_output(['readelf','-l',sys.argv[2]],text=True)
PY
cp "$pilon_source/LICENSE" "$pilon_licenses/LICENSE.txt"
cp "$pilon_source/NOTICE" "$pilon_licenses/NOTICE.txt"
cp "$pilon_patch" "$pilon_licenses/android-shell.patch"
cp "$pilon_work/codex-musl-tools-$TARGET/openssl-3.6.4/openssl-3.6.4/LICENSE.txt" "$pilon_licenses/OpenSSL-LICENSE.txt"
cp "$RUSTUP_HOME/toolchains/1.95.0-x86_64-unknown-linux-gnu/share/doc/rust/COPYRIGHT-library.html" "$pilon_licenses/Rust-COPYRIGHT-library.html"
cp -R "$RUSTUP_HOME/toolchains/1.95.0-x86_64-unknown-linux-gnu/share/doc/rust/licenses" "$pilon_licenses/"
cargo +1.95.0 metadata --locked --manifest-path "$pilon_source/codex-rs/Cargo.toml" --format-version 1 > "$pilon_work/dependencies.json"
python3 - "$pilon_work/dependencies.json" "$pilon_licenses/dependencies" <<'PY'
from pathlib import Path
import json, shutil, sys
for package in json.loads(Path(sys.argv[1]).read_text())['packages']:
    if not package.get('source'): continue
    root = Path(package['manifest_path']).parent
    for source in root.rglob('*'):
        if not source.is_file() or not source.name.upper().startswith(('LICENSE','LICENCE','COPYING','COPYRIGHT','NOTICE')): continue
        target = Path(sys.argv[2])/(package['name']+'-'+package['version'])/source.relative_to(root)
        target.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(source,target)
PY
cat > "$pilon_licenses/SOURCE.txt" <<'EOF'
OpenAI Codex 0.167.0, commit 086ccc833bc9cf01024ad2dd6af7dbd421b6bf9a
https://github.com/openai/codex/tree/rust-v0.167.0
Android-only source build: default/fallback shell is /system/bin/sh.
The packaged Pilon DNS helper uses Android's public Bionic getaddrinfo API for HTTP and WebSocket DNS.
PILON_DNS_HELPER selects the immutable APK-native helper; TLS validation remains enabled.
Focused retained regressions: bash addons/assistant/scripts/android/build-codex.sh --test-dns
Rust 1.95.0, Zig 0.14.1 musl cross-compiler, OpenSSL 3.6.4 and upstream checksum-verified V8 artifacts.
The desktop Codex runtime is unchanged. android-shell.patch records the modification.
EOF
mv "$pilon_output.new" "$pilon_output"
mv "$pilon_dns_output.new" "$pilon_dns_output"
printf '%s\n%s\n%s\n' "$pilon_input" "$(sha256sum "$pilon_output" | cut -d' ' -f1)" \
    "$(sha256sum "$pilon_dns_output" | cut -d' ' -f1)" > "$pilon_work/verified-build"
