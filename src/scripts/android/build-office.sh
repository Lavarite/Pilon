#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
pilon_dev="${PILON_DEV_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/pilon-dev}"
work="$pilon_dev/dev-deps/android/office"
runtime="$pilon_dev/builds/android/runtime/arm64-v8a"
licenses="$pilon_dev/builds/android/licenses/office"
office="$pilon_dev/builds/android/office"
sdk="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
ndk="${ANDROID_NDK_ROOT:-$sdk/ndk/27.2.12479018}"
java="${PILON_ANDROID_JAVA:-$HOME/Android/jdk-21}"
archive="$pilon_dev/dev-deps/downloads/libreoffice-26.2.6.3-arm64.apk"
upstream=https://raw.githubusercontent.com/LibreOffice/core/libreoffice-26.2.6.3
mkdir -p "$work/include" "$runtime" "$licenses" "$office/assets" "$(dirname "$archive")"
fetch() {
    local url=$1 destination=$2 checksum=$3
    if [[ ! -f "$destination" ]]; then
        curl --fail --location --retry 3 "$url" -o "$destination.new"
        mv "$destination.new" "$destination"
    fi
    printf '%s  %s\n' "$checksum" "$destination" | sha256sum --check --status
}
fetch https://f-droid.org/repo/org.documentfoundation.libreoffice_131.apk "$archive" 751ac3836890b79a56801bf6d9a2788cda3761ab5131a4f0b6abfbad822bb475
JAVA_HOME="$java" PATH="$java/bin:$PATH" "$sdk/build-tools/37.0.0/apksigner" verify --print-certs "$archive" > "$work/apk-signature.txt" 2>&1
rg -q 'certificate SHA-256 digest: af71ba344cab259fa625f079ff520f511d17a372bb2e8fa27532791bd5fea8cb' "$work/apk-signature.txt"
fetch "$upstream/include/LibreOfficeKit/LibreOfficeKit.h" "$work/include/LibreOfficeKit.h" d3828a4aa8fb61bd631e841d646b54939402a4be96addb90421d12ad2711f471
fetch "$upstream/include/LibreOfficeKit/LibreOfficeKitTypes.h" "$work/include/LibreOfficeKitTypes.h" 28700c222ef85b82a6b061f20516872f89c6bee4d0cbeb21de575793c1540064
fetch "$upstream/COPYING" "$licenses/COPYING" 8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903
fetch "$upstream/COPYING.MPL" "$licenses/COPYING.MPL" 1f256ecad192880510e84ad60474eab7589218784b9a50bc7ceee34c2b91f1d5
fetch "$upstream/COPYING.LGPL" "$licenses/COPYING.LGPL" a853c2ffec17057872340eee242ae4d96cbf2b520ae27d903e1b2fef1a5f9d1c
fetch "$upstream/readlicense_oo/license/license.xml" "$licenses/third-party-notices.html" 56fed7b93538da5d04a193ffd1dcf815c97a1efc4ce516eb2534a2f1f03b59a9
python3 - "$archive" "$runtime" "$office" <<'PY'
from pathlib import Path, PurePosixPath
from zipfile import ZipFile
import subprocess, sys, tempfile
archive, runtime, office = map(Path, sys.argv[1:])
with ZipFile(archive) as package, tempfile.TemporaryDirectory() as temporary:
    libraries = []
    for entry in package.infolist():
        name = PurePosixPath(entry.filename)
        if name.is_absolute() or '..' in name.parts: raise ValueError('Unsafe archive path')
        if entry.filename.startswith('lib/arm64-v8a/') and name.name != 'libc++_shared.so':
            path = Path(temporary) / name.name
            path.write_bytes(package.read(entry))
            rows = subprocess.check_output(['readelf', '-lW', str(path)], text=True).splitlines()
            alignments = [int(row.split()[-1], 16) for row in rows if row.strip().startswith('LOAD')]
            if not alignments or min(alignments) < 16384: raise ValueError('Office library is not 16KB aligned: ' + name.name)
            libraries.append(path)
        elif entry.filename.startswith('assets/') and not entry.filename.startswith('assets/dexopt/') and not entry.is_dir():
            data = package.read(entry)
            if entry.filename == 'assets/share/registry/writer.xcd':
                # Android registers TextDocument, but the default HTML WebDocument factory is omitted.
                start = data.index(b'<node oor:name="HTML" oor:op="replace">')
                end = data.index(b'</node>', start)
                block = data[start:end]
                original = b'<prop oor:name="DocumentService"><value>com.sun.star.text.WebDocument</value></prop>'
                replacement = b'<prop oor:name="DocumentService"><value>com.sun.star.text.TextDocument</value></prop>'
                if block.count(original) != 1: raise ValueError('Unexpected LibreOffice HTML filter definition')
                data = data[:start] + block.replace(original, replacement) + data[end:]
            destination = office / entry.filename
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
    for path in libraries:
        destination = runtime / path.name
        replacement = destination.with_suffix('.so.new')
        replacement.write_bytes(path.read_bytes())
        replacement.replace(destination)
PY
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang++" -std=c++23 -O2 -fPIC -shared \
    -I"$work/include" "$root/src/platform/android/office/main.cpp" -ldl -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 \
    -o "$runtime/libpilon_office.so.new"
mv "$runtime/libpilon_office.so.new" "$runtime/libpilon_office.so"
cat > "$licenses/SOURCE.txt" <<'EOF'
LibreOffice 26.2.6.3 Android ARM64, F-Droid reproducible source build
APK: https://f-droid.org/repo/org.documentfoundation.libreoffice_131.apk
APK SHA256: 751ac3836890b79a56801bf6d9a2788cda3761ab5131a4f0b6abfbad822bb475
APK signer SHA256: af71ba344cab259fa625f079ff520f511d17a372bb2e8fa27532791bd5fea8cb
Complete corresponding source: https://f-droid.org/repo/org.documentfoundation.libreoffice_131_src.tar.gz
Source tag: https://github.com/LibreOffice/core/tree/libreoffice-26.2.6.3
Build recipe: https://gitlab.com/fdroid/fdroiddata/-/blob/master/metadata/org.documentfoundation.libreoffice.yml
Pilon uses the unmodified engine and its current shared libc++ runtime.
The private extracted fontconfig cache path is adjusted for Pilon's package.
Android's HTML filter selects the existing Writer TextDocument service because
the Android native constructor list omits the WebDocument factory. That one
writer.xcd property is the only packaged upstream configuration change.
The full upstream licensing document includes the bundled third-party notices.
EOF
printf 'Office runtime: %s\nOffice assets: %s/assets\n' "$runtime" "$office"
