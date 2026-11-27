#!/usr/bin/env bash
set -euo pipefail
pilon_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$pilon_root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
pilon_dev="${PILON_DEV_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/pilon-dev}"
pilon_work="$pilon_dev/dev-deps/android/qt-platform"
pilon_kit="${PILON_ANDROID_QT:-$HOME/Qt/6.12.0/android_arm64_v8a}"
pilon_host="${QT_HOST_PATH:-$HOME/Qt/6.12.0/gcc_64}"
pilon_sdk="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
pilon_ndk="${ANDROID_NDK_ROOT:-$pilon_sdk/ndk/27.2.12479018}"
pilon_jdk="${JAVA_HOME:-$HOME/Android/jdk-21}"
pilon_base=025bdad181de241e81bf853c8a2d7bf3d19261a9
pilon_output="$pilon_dev/builds/android/qt-platform"
pilon_licenses="$pilon_dev/builds/android/licenses/qt-platform"
[[ "$("$pilon_kit/bin/qmake" -query QT_VERSION)" == 6.12.0 ]]
[[ "$("$pilon_host/bin/qmake" -query QT_VERSION)" == 6.12.0 ]]
mkdir -p "$pilon_work" "$pilon_output/plugins" "$pilon_licenses"
pilon_input="$(python3 - "$pilon_root" "$pilon_kit" "$pilon_ndk" <<'PY'
from pathlib import Path
import hashlib,sys
root,kit,ndk=map(Path,sys.argv[1:]);h=hashlib.sha256()
for p in [root/'src/scripts/android/build-qt-platform.sh',root/'src/platform/android/qt-platform/CMakeLists.txt',root/'src/platform/android/qt-platform/surface-lifecycle.patch',root/'src/platform/android/qt-platform/accessibility-activation.patch',root/'src/platform/android/qt-platform/ime-options.patch',root/'src/tests/platform/android/android_qt_ime_tests.py',root/'src/tests/platform/android/android_qt_texture_resize_tests.py',kit/'jar/Qt6Android.jar',kit/'lib/libQt6Core_arm64-v8a.so',kit/'lib/libQt6Gui_arm64-v8a.so',ndk/'source.properties']:
 h.update(hashlib.sha256(p.read_bytes()).digest())
print(h.hexdigest())
PY
)"
if python3 - "$pilon_output" "$pilon_input" <<'PY'
from pathlib import Path
import sys,json,hashlib
root=Path(sys.argv[1])
try:
 m=json.loads((root/'manifest.json').read_text())
 assert m['inputs']==sys.argv[2]
 for name,digest in m['files'].items():assert hashlib.sha256((root/name).read_bytes()).hexdigest()==digest
 for name,digest in m['license_files'].items():assert hashlib.sha256((root.parent/'licenses/qt-platform'/name).read_bytes()).hexdigest()==digest
except (OSError,ValueError,AssertionError,KeyError):sys.exit(1)
PY
then
    printf 'Android Qt surface backport is current.\n'
    exit 0
fi
if [[ ! -d "$pilon_work/upstream/.git" ]]; then
    git clone --depth 1 --branch v6.12.0 --single-branch https://github.com/qt/qtbase.git "$pilon_work/upstream"
fi
[[ "$(git -C "$pilon_work/upstream" rev-parse HEAD)" == "$pilon_base" ]]
git -C "$pilon_work/upstream" diff --quiet
python3 - "$pilon_work" "$pilon_base" "$pilon_input" "$pilon_root" <<'PY'
from pathlib import Path
import io,shutil,subprocess,sys,tarfile
work=Path(sys.argv[1]);root=Path(sys.argv[4]);source=work/'source'
if not source.exists() or not (source/'.pilon-source-stamp').exists() or (source/'.pilon-source-stamp').read_text()!=sys.argv[3]:
 if source.exists():
  assert source.name=='source' and source.parent.name=='qt-platform' and (source/'.pilon-source-stamp').exists()
  shutil.rmtree(source)
 source.mkdir()
 data=subprocess.check_output(['git','-C',str(work/'upstream'),'archive',sys.argv[2]])
 with tarfile.open(fileobj=io.BytesIO(data)) as tar:tar.extractall(source,filter='data')
 subprocess.run(['patch','--batch','--fuzz=0','-p1','-d',str(source),'-i',str(root/'src/platform/android/qt-platform/surface-lifecycle.patch')],check=True)
 subprocess.run(['patch','--batch','--fuzz=0','-p1','-d',str(source),'-i',str(root/'src/platform/android/qt-platform/accessibility-activation.patch')],check=True)
 subprocess.run(['patch','--batch','--fuzz=0','-p1','-d',str(source),'-i',str(root/'src/platform/android/qt-platform/ime-options.patch')],check=True)
 (source/'.pilon-source-stamp').write_text(sys.argv[3])
PY
python3 "$pilon_root/src/tests/platform/android/android_qt_ime_tests.py" \
    "$pilon_work/source/src/android/jar/src/org/qtproject/qt/android/QtInputConnection.java" \
    --jdk "$pilon_jdk" --android-jar "$pilon_sdk/platforms/android-37.0/android.jar"
python3 "$pilon_root/src/tests/platform/android/android_qt_texture_resize_tests.py" \
    "$pilon_work/source/src/android/jar/src/org/qtproject/qt/android/QtTextureView.java" --jdk "$pilon_jdk"
"$pilon_kit/bin/qt-cmake" -S "$pilon_root/src/platform/android/qt-platform" -B "$pilon_work/reproducible-build" -GNinja \
    -DCMAKE_BUILD_TYPE=Release -DQT_HOST_PATH="$pilon_host" -DANDROID_SDK_ROOT="$pilon_sdk" \
    -DANDROID_NDK_ROOT="$pilon_ndk" -DANDROID_PLATFORM=android-28 -DANDROID_ABI=arm64-v8a \
    -DPILON_QT_BASE_SOURCE="$pilon_work/source"
cmake --build "$pilon_work/reproducible-build" --parallel "${PILON_BUILD_JOBS:-4}"
python3 - "$pilon_root" "$pilon_kit" "$pilon_jdk" "$pilon_sdk" "$pilon_input" "$pilon_dev" <<'PY'
from pathlib import Path
import hashlib,json,os,shutil,struct,subprocess,sys,zipfile
root,kit,jdk,sdk=map(Path,sys.argv[1:5]);dev=Path(sys.argv[6]);work=dev/'dev-deps/android/qt-platform';out=dev/'builds/android/qt-platform';notices=dev/'builds/android/licenses/qt-platform'
classes=work/'reproducible-java'
if classes.exists():
 assert classes.name=='reproducible-java' and classes.parent.name=='qt-platform'
 shutil.rmtree(classes)
classes.mkdir()
folder=work/'source/src/android/jar/src/org/qtproject/qt/android'
names=['QtSurface','QtSurfaceInterface','QtTextureView','QtWindow','QtInputConnection']
subprocess.run([str(jdk/'bin/javac'),'-source','17','-target','17','-classpath',str(sdk/'platforms/android-37.0/android.jar')+':'+str(kit/'jar/Qt6Android.jar'),'-d',str(classes)]+[str(folder/(n+'.java')) for n in names],check=True)
jar=work/'reproducible-Qt6Android.jar';prefixes=['org/qtproject/qt/android/'+n for n in names]
with zipfile.ZipFile(kit/'jar/Qt6Android.jar') as original,zipfile.ZipFile(jar,'w',zipfile.ZIP_DEFLATED) as patched:
 for item in original.infolist():
  if any(item.filename==p+'.class' or item.filename.startswith(p+'$') for p in prefixes):continue
  patched.writestr(item,original.read(item))
 for p in sorted(classes.rglob('*.class')):
  if not any(str(p.relative_to(classes))==prefix+'.class' or str(p.relative_to(classes)).startswith(prefix+'$') for prefix in prefixes):continue
  info=zipfile.ZipInfo(str(p.relative_to(classes)),(2026,1,1,0,0,0));info.compress_type=zipfile.ZIP_DEFLATED
  patched.writestr(info,p.read_bytes())
plugin=work/'reproducible-build/libplugins_platforms_qtforandroid_arm64-v8a.so';data=plugin.read_bytes()
assert data[:6]==b'\x7fELF\x02\x01'
offset=struct.unpack_from('<Q',data,32)[0];size,count=struct.unpack_from('<HH',data,54)
loads=[struct.unpack_from('<Q',data,offset+i*size+48)[0] for i in range(count) if struct.unpack_from('<I',data,offset+i*size)[0]==1]
assert loads and min(loads)>=16384
files={}
for src,rel in [(plugin,'plugins/'+plugin.name),(jar,'pilon-Qt6Android.jar')]:
 target=out/rel;tmp=target.with_name(target.name+'.new');shutil.copyfile(src,tmp);os.replace(tmp,target);files[rel]=hashlib.sha256(target.read_bytes()).hexdigest()
for p in (work/'source/LICENSES').glob('*'):
 if p.is_file():shutil.copyfile(p,notices/p.name)
shutil.copyfile(root/'src/platform/android/qt-platform/surface-lifecycle.patch',notices/'surface-lifecycle.patch')
shutil.copyfile(root/'src/platform/android/qt-platform/accessibility-activation.patch',notices/'accessibility-activation.patch')
shutil.copyfile(root/'src/platform/android/qt-platform/ime-options.patch',notices/'ime-options.patch')
(notices/'SOURCE.txt').write_text('Qt6.12.0 source https://github.com/qt/qtbase/tree/025bdad181de241e81bf853c8a2d7bf3d19261a9\nOfficial upstream commits a1b6c8feb1db2d34daaa5450bd92df0cbe7b774c, 695347b645c308ca7c08f4b7149f2f1548c51296, 62311c96d897c45aee6900de1f0531506d5f8e20, a1ac05e2b7092e15edec738fc59fc8962358410f, f31445f99717c06cbca904d752d9fe47e88ca338.\nAdditional unmodified upstream fix https://github.com/qt/qtbase/commit/4708e204c2875a6b7427d521862816ade71e0dec marshals accessibility activation onto the Qt thread.\nLocal ime-options.patch makes QtInputConnection honor NO_ENTER_ACTION independently of other EditorInfo flags, preserving NO_FULLSCREEN; it masks action bits for Next/Previous/Done.\nPinned original https://github.com/qt/qtbase/blob/025bdad181de241e81bf853c8a2d7bf3d19261a9/src/android/jar/src/org/qtproject/qt/android/QtInputConnection.java and Android contract https://developer.android.com/reference/android/view/inputmethod/EditorInfo#IME_FLAG_NO_ENTER_ACTION\nThe backport preserves 6.12.0 longPress and Insets APIs. Local TextureView resize handling queues native surface teardown and recreation while retaining the Java Surface/SurfaceTexture. Android requires EGL surface recreation after buffer-size changes: https://developer.android.com/reference/android/graphics/SurfaceTexture#setDefaultBufferSize(int,%20int)\nOnly the Android platform plugin and five Java classes are rebuilt against the installed exact-version kit. Global kit and QtCore/Gui are unchanged.\nThe complete applied source patches and upstream license texts are included here; src/scripts/android/build-qt-platform.sh reproduces both artifacts.\n')
assert (notices/'LGPL-3.0-only.txt').is_file()
license_files={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in notices.iterdir() if p.is_file()}
manifest={'inputs':sys.argv[5],'license_files':license_files,'qt_base_commit':'025bdad181de241e81bf853c8a2d7bf3d19261a9','patch_sha256':hashlib.sha256((root/'src/platform/android/qt-platform/surface-lifecycle.patch').read_bytes()).hexdigest(),'activation_patch_sha256':hashlib.sha256((root/'src/platform/android/qt-platform/accessibility-activation.patch').read_bytes()).hexdigest(),'ime_options_patch_sha256':hashlib.sha256((root/'src/platform/android/qt-platform/ime-options.patch').read_bytes()).hexdigest(),'files':files,'load_alignment':loads,'device_verified':False}
(out/'manifest.json.new').write_text(json.dumps(manifest,indent=2)+'\n');os.replace(out/'manifest.json.new',out/'manifest.json')
print('Android Qt surface backport artifacts verified and staged.')
PY
