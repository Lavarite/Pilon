#!/usr/bin/env bash
set -euo pipefail

pilon_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$pilon_root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
pilon_work="$pilon_root/build/android/git-runtime"
pilon_output="$pilon_root/builds/android/runtime/arm64-v8a"
pilon_android_build="${PILON_ANDROID_BUILD_DIR:-$pilon_root/build/android/app}"
pilon_ndk="${ANDROID_NDK_ROOT:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}/ndk/27.2.12479018}"
pilon_tools="$pilon_ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
pilon_ssl="$pilon_android_build/_deps/pilon_openssl-build"
pilon_ssl_source="$pilon_android_build/_deps/pilon_openssl-src"
pilon_licenses="$pilon_root/builds/android/licenses/git"
mkdir -p "$pilon_work/downloads" "$pilon_output" "$pilon_licenses"
[[ -f "$pilon_ssl/libcrypto_3.so" ]] || { printf 'Build pilon_android_openssl first.\n' >&2; exit 1; }

download() {
    local pilon_url="$1" pilon_file="$2" pilon_sha="$3"
    if [[ ! -f "$pilon_file" ]]; then
        curl --fail --location --retry 3 --output "$pilon_file.part" "$pilon_url"
        printf '%s  %s\n' "$pilon_sha" "$pilon_file.part" | sha256sum --check
        mv "$pilon_file.part" "$pilon_file"
    fi
    printf '%s  %s\n' "$pilon_sha" "$pilon_file" | sha256sum --check
}

# Publisher digests: kernel.org's sha256sums.asc and github.com/curl/curl/releases/tag/curl-8_23_0.
download https://www.kernel.org/pub/software/scm/git/git-2.56.0.tar.xz \
    "$pilon_work/downloads/git-2.56.0.tar.xz" \
    26c56c296b38c0695b26fa95f475f1d01704d2d38e73465ca30b0b2f5dc789d3
download https://curl.se/download/curl-8.23.0.tar.xz \
    "$pilon_work/downloads/curl-8.23.0.tar.xz" \
    8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5
[[ -d "$pilon_work/curl-8.23.0" ]] || tar -xJf "$pilon_work/downloads/curl-8.23.0.tar.xz" -C "$pilon_work"
# CMake and Ninja metadata contain absolute build paths; keep them together.
if [[ -d "$pilon_work/curl-build" ]] && { [[ ! -f "$pilon_work/curl-build/CMakeCache.txt" ]] \
    || [[ "$(sed -n 's/^CMAKE_CACHEFILE_DIR:INTERNAL=//p' "$pilon_work/curl-build/CMakeCache.txt")" != "$pilon_work/curl-build" ]]; }; then
    mkdir -p "$pilon_root/build/archive/cmake-relocation"
    pilon_archive="$(mktemp -d "$pilon_root/build/archive/cmake-relocation/curl-build.XXXXXX")"
    mv -- "$pilon_work/curl-build" "$pilon_archive/build"
fi
cmake -S "$pilon_work/curl-8.23.0" -B "$pilon_work/curl-build" -GNinja \
    -DCMAKE_TOOLCHAIN_FILE="$pilon_ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$pilon_work/deps" -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
    -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DCURL_USE_OPENSSL=ON \
    -DOPENSSL_CRYPTO_LIBRARY="$pilon_ssl/libcrypto_3.so" -DOPENSSL_SSL_LIBRARY="$pilon_ssl/libssl_3.so" \
    -DOPENSSL_INCLUDE_DIR="$pilon_ssl_source/include" -DCMAKE_C_FLAGS="-I$pilon_ssl/include" \
    -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DUSE_NGHTTP2=OFF \
    -DCURL_CA_PATH=none -DCURL_CA_BUNDLE=none
cmake --build "$pilon_work/curl-build" --target install --parallel "${PILON_BUILD_JOBS:-4}"

pilon_patch_sha="$(sha256sum "$pilon_root/addons/git/platform/android/git.patch" | cut -d' ' -f1)"
if [[ ! -f "$pilon_work/git-2.56.0/.pilon-patch" ]] \
    || [[ "$(cat "$pilon_work/git-2.56.0/.pilon-patch")" != "$pilon_patch_sha" ]]; then
    rm -rf -- "$pilon_work/git-2.56.0"
    tar -xJf "$pilon_work/downloads/git-2.56.0.tar.xz" -C "$pilon_work"
    patch -d "$pilon_work/git-2.56.0" -p1 < "$pilon_root/addons/git/platform/android/git.patch"
    printf '%s\n' "$pilon_patch_sha" > "$pilon_work/git-2.56.0/.pilon-patch"
fi
# Native C implementations preserve the app's Git commands; Perl/Tcl frontends are not used by Pilon.
make -C "$pilon_work/git-2.56.0" -j"${PILON_BUILD_JOBS:-4}" git git-remote-http \
    CC="$pilon_tools/aarch64-linux-android28-clang" AR="$pilon_tools/llvm-ar" \
    SHELL=/bin/sh SHELL_PATH=/system/bin/sh prefix=/data/user/0/org.pilon.Pilon/files/git \
    NO_GETTEXT=1 NO_PERL=1 NO_TCLTK=1 NO_PYTHON=1 NO_RUST=1 NO_EXPAT=1 NO_GECOS_IN_PWENT=1 \
    CURLDIR="$pilon_work/deps" CURL_LDFLAGS="-lcurl -lssl -lcrypto -lz -ldl" \
    OPENSSLDIR="$pilon_ssl" CFLAGS="-O2 -fPIE -I$pilon_ssl_source/include" \
    LDFLAGS="-L$pilon_ssl -Wl,-z,max-page-size=16384,-z,common-page-size=16384 -Wl,-rpath,'\$\$ORIGIN'" \
    HAVE_SYSINFO= CSPRNG_METHOD=getrandom PTHREAD_LIBS=
for pilon_program in git git-remote-http; do
    pilon_name="${pilon_program//-/_}"
    install -m755 "$pilon_work/git-2.56.0/$pilon_program" "$pilon_output/libpilon_$pilon_name.so.new"
    "$pilon_tools/llvm-strip" --strip-unneeded "$pilon_output/libpilon_$pilon_name.so.new"
    mv "$pilon_output/libpilon_$pilon_name.so.new" "$pilon_output/libpilon_$pilon_name.so"
done
cp "$pilon_work/git-2.56.0/COPYING" "$pilon_licenses/Git-COPYING.txt"
cp "$pilon_work/git-2.56.0/sha1dc/LICENSE.txt" "$pilon_licenses/SHA1DC-LICENSE.txt"
cp "$pilon_work/curl-8.23.0/COPYING" "$pilon_licenses/curl-COPYING.txt"
printf 'Android Git helpers: %s\n' "$pilon_output"
