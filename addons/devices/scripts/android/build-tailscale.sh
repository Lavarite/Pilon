#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
work="$root/build/android/tailscale"
source="$work/upstream"
patched="$work/without-taildrop"
commit=d8673104f600c05b8c20b64d0e70951ed670236a
sdk="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
ndk="${ANDROID_NDK_ROOT:-$sdk/ndk/27.2.12479018}"
licenses="$root/builds/android/licenses/tailscale"
mkdir -p "$work/bin" "$licenses" "$root/builds/android/java"
if [[ ! -d "$source/.git" ]]; then
    git clone --branch 1.104.5-t788384763-gd8673104f --depth 1 --single-branch \
        https://github.com/tailscale/tailscale-android.git "$source"
fi
[[ $(git -C "$source" rev-parse HEAD) == "$commit" ]]
git -C "$source" diff --quiet
python3 - "$source" "$patched" "$commit" <<'PY'
from pathlib import Path
import io,shutil,subprocess,sys,tarfile
source,target,commit=Path(sys.argv[1]),Path(sys.argv[2]),sys.argv[3]
assert target.name=='without-taildrop' and target.parent.name=='tailscale'
if target.exists():
    assert (target/'.pilon-build-copy').read_text()==commit
    shutil.rmtree(target)
target.mkdir()
archive=subprocess.check_output(['git','-C',str(source),'archive',commit])
with tarfile.open(fileobj=io.BytesIO(archive)) as files:
    files.extractall(target,filter='data')
(target/'.pilon-build-copy').write_text(commit)
PY
git -C "$patched" apply "$root/addons/devices/platform/android/tailscale/omit-taildrop.patch"
[[ $(go env GOVERSION) == go1.27.3* ]]
export GOTOOLCHAIN=local ANDROID_HOME="$sdk" ANDROID_NDK_HOME="$ndk" GOBIN="$work/bin"
export JAVA_HOME="${PILON_ANDROID_JAVA_HOME:-$HOME/Android/jdk-21}"
export PATH="$JAVA_HOME/bin:$work/bin:$PATH"
(cd "$source"; go run tailscale.com/cmd/mkversion) > "$patched/tailscale.version"
(
    cd "$patched"
    GOOS=android GOARCH=arm64 CGO_ENABLED=1 go list -tags not_tailscale_go,ts_omit_cachenetmap,ts_omit_taildrop \
        -deps ./libtailscale > "$work/packages.txt"
    if rg -q '^tailscale.com/feature/taildrop($|/)' "$work/packages.txt"; then
        printf 'Taildrop was unexpectedly linked into the Android SDK.\n' >&2
        exit 1
    fi
    go install golang.org/x/mobile/cmd/gobind golang.org/x/mobile/cmd/gomobile
    gomobile bind -target android/arm64 -androidapi 28 -tags not_tailscale_go,ts_omit_cachenetmap,ts_omit_taildrop \
        -ldflags "-linkmode=external -extldflags=-Wl,-z,max-page-size=16384 $(./version-ldflags.sh)" \
        -o "$work/libtailscale.aar" ./libtailscale
    GOOS=android GOARCH=arm64 CGO_ENABLED=1 go list -tags not_tailscale_go,ts_omit_cachenetmap,ts_omit_taildrop \
        -deps -f '{{if .Module}}{{.Module.Dir}}{{end}}' ./libtailscale | sort -u > "$work/modules.txt"
)
python3 - "$work" "$licenses" "$(go env GOROOT)" <<'PY'
from pathlib import Path
import shutil,subprocess,sys,zipfile
work,licenses,goroot=map(Path,sys.argv[1:])
with zipfile.ZipFile(work/'libtailscale.aar') as archive:
    native=work/'libgojni.so';native.write_bytes(archive.read('jni/arm64-v8a/libgojni.so'))
segments=[row for row in subprocess.check_output(['readelf','-lW',native],text=True).splitlines() if row.strip().startswith('LOAD')]
assert segments and min(int(row.split()[-1],16) for row in segments)>=16384
modules=[Path(line) for line in (work/'modules.txt').read_text().splitlines() if line]
modules.extend([work/'upstream',goroot])
for module in modules:
    name='go-runtime' if module==goroot else ('tailscale-android' if module in (work/'upstream',work/'without-taildrop') else str(module).split('/pkg/mod/')[-1])
    copied=False
    for file in module.iterdir():
        if file.is_file() and file.name.lower().split('.')[0] in ('license','copying','notice','authors','patents'):
            target=licenses/name/file.name;target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(file,target);copied=True
    if module==goroot:
        if not copied:
            file=Path('/usr/share/licenses/go/LICENSE')
            target=licenses/name/'LICENSE';target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(file,target);copied=True
        for file in (goroot/'src/vendor').rglob('*'):
            if file.is_file() and file.name.lower().split('.')[0] in ('license','copying','notice','authors','patents'):
                target=licenses/name/'vendor'/file.relative_to(goroot/'src/vendor');target.parent.mkdir(parents=True,exist_ok=True)
                shutil.copyfile(file,target)
    assert copied, f'No license found for compiled module {module}'
PY
cp "$work/libtailscale.aar" "$root/builds/android/java/libtailscale.aar.new"
mv "$root/builds/android/java/libtailscale.aar.new" "$root/builds/android/java/libtailscale.aar"
cp "$root/addons/devices/platform/android/tailscale/Gio-LICENSE.txt" "$licenses/Gio-LICENSE.txt"
cp "$root/addons/devices/platform/android/tailscale/omit-taildrop.patch" "$licenses/SDK-omit-taildrop.patch"
cat > "$licenses/SOURCE.txt" <<EOF
Tailscale Android 1.104.5-t788384763-gd8673104f, core 1.104.5
https://github.com/tailscale/tailscale-android/tree/$commit
Official libtailscale gomobile engine, stock Go 1.27.3, arm64 API28, 16KB ELF.
Build tags: not_tailscale_go,ts_omit_cachenetmap,ts_omit_taildrop.
Pilon applies addons/devices/platform/android/tailscale/omit-taildrop.patch to an isolated SDK build copy:
remove the optional backend Taildrop setup/import and compile its FileOps only when
ts_omit_taildrop is absent. The pinned upstream clone and core are unmodified.
Patch SHA256: $(sha256sum "$root/addons/devices/platform/android/tailscale/omit-taildrop.patch" | cut -d' ' -f1)
The build asserts feature/taildrop is absent from the actual package dependency graph.
Go's Android c-shared startup has an empty envv, so libc/Java setenv cannot control
feature package initialization; no TS_DISABLE_FEATURE environment workaround is used.
https://github.com/golang/go/blob/go1.27.3/src/runtime/rt0_android_arm64.s
Platform adapters derive narrowly from upstream BSD-3-Clause App.kt/IPNService.kt.
Compiled Go-module licenses and Go runtime notices are retained in this directory.
Upstream libtailscale/log.go borrows MIT-licensed Gio logging code; notice retained from
https://github.com/gioui/gio/blob/3f4f8ba7c1fae349f97621f1f623ef47e4da3e25/LICENSE
EOF
printf 'Tailscale AAR: %s\n' "$root/builds/android/java/libtailscale.aar"
