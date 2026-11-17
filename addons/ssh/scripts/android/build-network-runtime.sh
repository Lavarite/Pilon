#!/usr/bin/env bash
set -euo pipefail

pilon_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$pilon_root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
pilon_work="$pilon_root/build/android/network-runtime"
pilon_output="$pilon_root/builds/android/runtime/arm64-v8a"
pilon_android_build="${PILON_ANDROID_BUILD_DIR:-$pilon_root/build/android/app}"
pilon_ndk="${ANDROID_NDK_ROOT:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}/ndk/27.2.12479018}"
pilon_toolchain="$pilon_ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
pilon_ssl="$pilon_android_build/_deps/pilon_openssl-build"
pilon_ssl_source="$pilon_android_build/_deps/pilon_openssl-src"
pilon_licenses="$pilon_root/builds/android/licenses/ssh"
mkdir -p "$pilon_work/downloads" "$pilon_output" "$pilon_licenses"
[[ -f "$pilon_ssl/libcrypto.a" ]] || { printf 'Build pilon_android_openssl before the SSH runtime.\n' >&2; exit 1; }

download() {
    local pilon_url="$1" pilon_file="$2" pilon_sha="$3"
    if [[ ! -f "$pilon_file" ]]; then
        curl --fail --location --retry 3 --output "$pilon_file.part" "$pilon_url"
        printf '%s  %s\n' "$pilon_sha" "$pilon_file.part" | sha256sum --check
        mv "$pilon_file.part" "$pilon_file"
    fi
    printf '%s  %s\n' "$pilon_sha" "$pilon_file" | sha256sum --check
}

archive_build() {
    [[ -d "$1" ]] || return 0
    local pilon_archive
    mkdir -p "$pilon_root/build/archive/autoconf"
    pilon_archive="$(mktemp -d "$pilon_root/build/archive/autoconf/$(basename "$1").XXXXXX")"
    mv -- "$1" "$pilon_archive/build"
}

# OpenSSH's release digest is published at https://www.openssh.com/txt/release-10.6p2.
download https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/openssh-10.6p2.tar.gz \
    "$pilon_work/downloads/openssh-10.6p2.tar.gz" \
    9ae44872090cf2f06b3cf9ff147e7f16f309671340763ed2f99248398ae29d5b
# Android's resolver headers need the same ldns path used by the upstream Termux package.
download https://www.nlnetlabs.nl/downloads/ldns/ldns-1.9.2.tar.gz \
    "$pilon_work/downloads/ldns-1.9.2.tar.gz" \
    b524fa21994b6e834200ceb8c27f1b84bda5982fe35706f058196c079db94d5d

export CC="$pilon_toolchain/aarch64-linux-android28-clang"
export AR="$pilon_toolchain/llvm-ar" RANLIB="$pilon_toolchain/llvm-ranlib" STRIP="$pilon_toolchain/llvm-strip"
export CPPFLAGS="-I$pilon_ssl/include -I$pilon_ssl_source/include"
export LDFLAGS="-L$pilon_ssl -Wl,-z,max-page-size=16384,-z,common-page-size=16384"
export LIBS="-lssl -lcrypto -ldl"

[[ -d "$pilon_work/ldns-1.9.2" ]] || tar -xzf "$pilon_work/downloads/ldns-1.9.2.tar.gz" -C "$pilon_work"
pilon_ldns_config="$( { printf '%s\n' "$pilon_work" "$pilon_ssl" "$pilon_ssl_source" "$pilon_ndk";
    sha256sum "$pilon_root/addons/ssh/scripts/android/build-network-runtime.sh" "$pilon_ssl/configdata.pm"; } | sha256sum | cut -d' ' -f1)"
if [[ ! -f "$pilon_work/ldns-build/.pilon-config" ]] \
    || [[ "$(cat "$pilon_work/ldns-build/.pilon-config")" != "$pilon_ldns_config" ]]; then
    archive_build "$pilon_work/ldns-build"
    mkdir -p "$pilon_work/ldns-build/compat"
    ( cd "$pilon_work/ldns-build"
      CFLAGS="-O2 -fPIC" "$pilon_work/ldns-1.9.2/configure" --host=aarch64-linux-android \
          --prefix="$pilon_work/deps" --disable-shared --enable-static --disable-gost --with-ssl="$pilon_ssl" )
    printf '%s\n' "$pilon_ldns_config" > "$pilon_work/ldns-build/.pilon-config"
fi
make -C "$pilon_work/ldns-build" -j"${PILON_BUILD_JOBS:-4}" install

pilon_patch_sha="$(sha256sum "$pilon_root/addons/ssh/platform/android/openssh.patch" | cut -d' ' -f1)"
if [[ ! -f "$pilon_work/openssh-10.6p2/.pilon-patch" ]] \
    || [[ "$(cat "$pilon_work/openssh-10.6p2/.pilon-patch")" != "$pilon_patch_sha" ]]; then
    rm -rf -- "$pilon_work/openssh-10.6p2" "$pilon_work/ssh-build"
    tar -xzf "$pilon_work/downloads/openssh-10.6p2.tar.gz" -C "$pilon_work"
    patch -d "$pilon_work/openssh-10.6p2" -p1 < "$pilon_root/addons/ssh/platform/android/openssh.patch"
    printf '%s\n' "$pilon_patch_sha" > "$pilon_work/openssh-10.6p2/.pilon-patch"
fi
mkdir -p "$pilon_work/ssh-build"
pilon_config_sha="$( { printf '%s\n' "$pilon_work" "$pilon_ssl" "$pilon_ssl_source" "$pilon_ndk";
    sha256sum "$pilon_root/addons/ssh/scripts/android/build-network-runtime.sh" "$pilon_ssl/configdata.pm" \
    "$pilon_ssl/libcrypto_3.so" "$pilon_ssl/libssl_3.so"; } | sha256sum | cut -d' ' -f1)"
if [[ ! -f "$pilon_work/ssh-build/.pilon-config" ]] \
    || [[ "$(cat "$pilon_work/ssh-build/.pilon-config")" != "$pilon_config_sha" ]]; then
    archive_build "$pilon_work/ssh-build"
    mkdir -p "$pilon_work/ssh-build"
    ( cd "$pilon_work/ssh-build"
      CFLAGS="-O2 -fPIE -Dfd_mask=int -DHAVE_ATTRIBUTE__SENTINEL__=1" \
      LDFLAGS="$LDFLAGS -Wl,-rpath,'\$\$ORIGIN'" \
      ac_cv_func_endgrent=yes ac_cv_func_fmt_scaled=no ac_cv_func_getlastlogxbyname=no \
      ac_cv_func_readpassphrase=no ac_cv_func_strnvis=no ac_cv_header_sys_un_h=yes \
      ac_cv_lib_crypt_crypt=no ac_cv_search_getrrsetbyname=no ac_cv_func_bzero=yes \
      ac_cv_member_struct_passwd_pw_gecos=no \
      "$pilon_work/openssh-10.6p2/configure" --host=aarch64-linux-android \
          --disable-etc-default-login --disable-lastlog --disable-libutil --disable-pututline \
          --disable-pututxline --disable-strip --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx \
          --sysconfdir=/data/user/0/org.pilon.Pilon/files/.ssh --with-default-path=/system/bin \
          --with-privsep-path=/data/user/0/org.pilon.Pilon/cache/ssh --without-pam --without-selinux \
          --with-ldns="$pilon_work/deps" --without-libedit --without-kerberos5 --without-security-key-builtin )
    printf '%s\n' "$pilon_config_sha" > "$pilon_work/ssh-build/.pilon-config"
fi
make -C "$pilon_work/ssh-build" -j"${PILON_BUILD_JOBS:-4}" ssh scp
for pilon_program in ssh scp; do
    install -m755 "$pilon_work/ssh-build/$pilon_program" "$pilon_output/libpilon_$pilon_program.so.new"
    "$STRIP" --strip-unneeded "$pilon_output/libpilon_$pilon_program.so.new"
    mv "$pilon_output/libpilon_$pilon_program.so.new" "$pilon_output/libpilon_$pilon_program.so"
done
cp "$pilon_work/openssh-10.6p2/LICENCE" "$pilon_licenses/OpenSSH-LICENCE.txt"
cp "$pilon_work/ldns-1.9.2/LICENSE" "$pilon_licenses/ldns-LICENSE.txt"
printf 'Android SSH and SCP helpers: %s\n' "$pilon_output"
