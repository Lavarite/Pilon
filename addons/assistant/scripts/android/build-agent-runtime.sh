#!/usr/bin/env bash
set -euo pipefail
pilon_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)"
if [[ "${PILON_ENV_LOADED:-0}" != 1 ]]; then
    exec python3 "$pilon_root/src/scripts/environment.py" bash "${BASH_SOURCE[0]}" "$@"
fi
pilon_work="$pilon_root/build/android/agent-runtime"
pilon_recipe="$pilon_root/addons/assistant/platform/android/agent-runtime"
pilon_sdk="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
pilon_ndk="${ANDROID_NDK_ROOT:-$pilon_sdk/ndk/27.2.12479018}"
pilon_output="$pilon_root/builds/android/agent-runtime"
pilon_python_sha=423c80b957330cf0ebb92b9b20f2294c292144a441bdb0ceb8c69d14928e569b
pilon_node_sha=7b9adc82d612d0a2a863d96c46de2292552e250cee51956ebbe0146c42aacae9
mkdir -p "$pilon_work/downloads" "$pilon_work/react" "$pilon_output/runtime/arm64-v8a" "$pilon_output/assets/agent-runtime"
pilon_inputs="$(python3 - "$pilon_recipe" "$pilon_root/addons/assistant/scripts/android/build-agent-runtime.sh" "$pilon_ndk" <<'PY'
from pathlib import Path
import hashlib,sys
recipe,script,ndk=map(Path,sys.argv[1:]);h=hashlib.sha256()
for p in [script,ndk/'source.properties',ndk/'sources/android/cpufeatures/cpu-features.c']+sorted(p for p in recipe.rglob('*') if p.is_file()):h.update(hashlib.sha256(p.read_bytes()).digest())
print(h.hexdigest())
PY
)"
if python3 - "$pilon_output" "$pilon_inputs" <<'PY'
from pathlib import Path
import json,sys,hashlib
root=Path(sys.argv[1])
try:
 m=json.loads((root/'manifest.json').read_text());assert m['inputs']==sys.argv[2]
 for name,digest in m['files'].items():assert hashlib.sha256((root/name).read_bytes()).hexdigest()==digest
 assert m['licenses_complete']
 for name,digest in m['license_files'].items():assert hashlib.sha256((root.parent/'licenses/agent-runtime'/name).read_bytes()).hexdigest()==digest
except (OSError,ValueError,AssertionError,KeyError):sys.exit(1)
PY
then
    printf 'Android Python/Node project runtime is current.\n'
    exit 0
fi
pilon_fetch() {
    local pilon_url="$1" pilon_path="$2" pilon_digest="$3"
    if [[ ! -f "$pilon_path" ]] || ! printf '%s  %s\n' "$pilon_digest" "$pilon_path" | sha256sum -c --status; then
        curl --fail --location --retry 3 "$pilon_url" -o "$pilon_path.new"
        printf '%s  %s\n' "$pilon_digest" "$pilon_path.new" | sha256sum -c --status
        mv "$pilon_path.new" "$pilon_path"
    fi
}
pilon_fetch https://www.python.org/ftp/python/3.14.10/python-3.14.10-aarch64-linux-android.tar.gz \
    "$pilon_work/downloads/python-3.14.10-aarch64-linux-android.tar.gz" "$pilon_python_sha"
pilon_fetch https://nodejs.org/dist/v22.23.4/node-v22.23.4.tar.xz \
    "$pilon_work/downloads/node-v22.23.4.tar.xz" "$pilon_node_sha"
python3 - "$pilon_work" <<'PY'
from pathlib import Path
import sys,tarfile
root=Path(sys.argv[1])
for archive,target,sentinel in [('python-3.14.10-aarch64-linux-android.tar.gz','python','prefix/lib/libpython3.14.so'),('node-v22.23.4.tar.xz','node-source','node-v22.23.4/configure.py')]:
 destination=root/target
 if not (destination/sentinel).is_file():
  destination.mkdir(exist_ok=True)
  with tarfile.open(root/'downloads'/archive) as tar:tar.extractall(destination,filter='data')
PY
# Regenerate only the two patched build files; retain compiled source objects.
python3 - "$pilon_work" "$pilon_recipe" "$pilon_ndk" <<'PY'
from pathlib import Path
import shutil,subprocess,sys,tarfile
work,recipe,ndk=map(Path,sys.argv[1:]);source=work/'node-source/node-v22.23.4'
with tarfile.open(work/'downloads/node-v22.23.4.tar.xz') as tar:
 for name in ['tools/v8_gypfiles/v8.gyp','deps/zlib/zlib.gyp']:
  (source/name).write_bytes(tar.extractfile('node-v22.23.4/'+name).read())
subprocess.run(['patch','--batch','--fuzz=0','-p1','-d',str(source),'-i',str(recipe/'node-android-build.patch')],check=True)
shutil.copyfile(ndk/'sources/android/cpufeatures/cpu-features.c',source/'deps/zlib/cpu-features-android.c')
PY
(
    cd "$pilon_work/node-source/node-v22.23.4"
    CC_host=/usr/bin/clang CXX_host=/usr/bin/clang++ LINK_host=/usr/bin/clang++ \
        ./android-configure "$pilon_ndk" 28 arm64
    python3 - "${PILON_BUILD_JOBS:-4}" <<'PY'
import subprocess,sys
subprocess.run(['make','-j'+sys.argv[1],"LDFLAGS=-Wl,-z,max-page-size=16384,-z,common-page-size=16384 -Wl,-rpath,'$$ORIGIN'"],check=True)
PY
)
cp "$pilon_recipe/package.json" "$pilon_recipe/package-lock.json" "$pilon_work/react/"
npm ci --prefix "$pilon_work/react" --ignore-scripts --omit=optional --no-audit --no-fund
python3 - "$pilon_root" "$pilon_ndk" "$pilon_inputs" <<'PY'
from pathlib import Path
import base64,hashlib,io,json,os,shutil,struct,subprocess,sys,tarfile,urllib.request,zipfile
root,ndk=map(Path,sys.argv[1:3]);work=root/'build/android/agent-runtime';recipe=root/'addons/assistant/platform/android/agent-runtime';out=root/'builds/android/agent-runtime';native=out/'runtime/arm64-v8a';assets=out/'assets/agent-runtime';prefix=work/'python/prefix';tools=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
licenses=root/'builds/android/licenses/agent-runtime';licenses.mkdir(parents=True,exist_ok=True)
source=work/'node-source/node-v22.23.4';stage=work/'native-stage'
if stage.exists():
 assert stage.name=='native-stage' and stage.parent.name=='agent-runtime'
 shutil.rmtree(stage)
stage.mkdir()
subprocess.run([str(tools/'aarch64-linux-android28-clang'),'-O2','-fPIE','-pie','-I'+str(prefix/'include/python3.14'),str(recipe/'python-main.c'),'-L'+str(prefix/'lib'),'-lpython3.14','-Wl,-z,max-page-size=16384,-z,common-page-size=16384','-Wl,-rpath,$ORIGIN','-o',str(stage/'libpilon_python.so')],check=True)
subprocess.run([str(tools/'llvm-strip'),'--strip-unneeded','-o',str(stage/'libpilon_node.so'),str(source/'out/Release/node')],check=True)
subprocess.run([str(tools/'aarch64-linux-android28-clang'),'-O2','-fPIE','-pie',str(recipe/'node-cli.c'),'-Wl,-z,max-page-size=16384,-z,common-page-size=16384','-o',str(stage/'libpilon_node_cli.so')],check=True)
lock=json.loads((recipe/'package-lock.json').read_text());esbuild=lock['packages']['node_modules/@esbuild/android-arm64']
upstream=work/'esbuild-upstream';commit='208f539945b145e7c9d6d844290f81c3fe5af320'
if not (upstream/'.git').exists():subprocess.run(['git','clone','--depth','1','--branch','v0.25.12','--single-branch','https://github.com/evanw/esbuild.git',str(upstream)],check=True)
assert subprocess.check_output(['git','-C',str(upstream),'rev-parse','HEAD'],text=True).strip()==commit
subprocess.run(['git','-C',str(upstream),'diff','--quiet'],check=True)
source_esbuild=work/'esbuild-production-source'
if source_esbuild.exists():
 assert source_esbuild.name=='esbuild-production-source' and source_esbuild.parent==work
 shutil.rmtree(source_esbuild)
source_esbuild.mkdir()
archive=subprocess.check_output(['git','-C',str(upstream),'archive',commit])
with tarfile.open(fileobj=io.BytesIO(archive)) as tar:tar.extractall(source_esbuild,filter='data')
subprocess.run(['patch','--batch','--fuzz=0','-p1','-d',str(source_esbuild),'-i',str(recipe/'esbuild-android-directory.patch')],check=True)
shutil.copyfile(recipe/'esbuild-android-directory-test.go',source_esbuild/'internal/fs/fs_android_directory_test.go')
go_env={**os.environ,'GOTOOLCHAIN':'go1.23.12','GOOS':'linux','GOARCH':'amd64','CGO_ENABLED':'0'}
subprocess.run(['go','test','-run','TestAndroidMaskedDirectoryKeepsDescendantTraversal','./internal/fs'],cwd=source_esbuild,env=go_env,check=True)
go_env.update(GOOS='android',GOARCH='arm64')
subprocess.run(['go','build','-trimpath','-buildvcs=false','-ldflags=-s -w -buildid=','-o',str(stage/'libpilon_esbuild.so'),'./cmd/esbuild'],cwd=source_esbuild,env=go_env,check=True)
shutil.copyfile(recipe/'esbuild-android-directory.patch',licenses/'esbuild-android-directory.patch')
extensions={}
for p in (prefix/'lib/python3.14/lib-dynload').glob('*.so'):
 name='libpilon_py_'+p.name;shutil.copyfile(p,stage/name);extensions[p.name]=name
for p in list((prefix/'lib').glob('lib*_python.so'))+[prefix/'lib/libpython3.14.so']:shutil.copyfile(p,stage/p.name)
files={}
for p in stage.glob('*.so'):
 data=p.read_bytes();assert data[:6]==b'\x7fELF\x02\x01' and struct.unpack_from('<H',data,18)[0]==183,p
 offset=struct.unpack_from('<Q',data,32)[0];size,count=struct.unpack_from('<HH',data,54);loads=[struct.unpack_from('<Q',data,offset+i*size+48)[0] for i in range(count) if struct.unpack_from('<I',data,offset+i*size)[0]==1]
 assert loads and min(loads)>=16384,(p,loads)
 target=native/p.name;tmp=target.with_name(target.name+'.new');shutil.copyfile(p,tmp);os.replace(tmp,target);files[str(target.relative_to(out))]=hashlib.sha256(data).hexdigest()
bundle=assets/'runtime.zip.new'
with zipfile.ZipFile(bundle,'w',zipfile.ZIP_DEFLATED) as zip:
 for name,folder in [('python/lib/python3.14',prefix/'lib/python3.14'),('node/node_modules',work/'react/node_modules'),('npm',source/'deps/npm')]:
  for p in sorted(folder.rglob('*')):
   if p.is_file() and not p.is_symlink() and not any(part in p.parts for part in ['__pycache__','lib-dynload','.bin']):
    info=zipfile.ZipInfo(name+'/'+str(p.relative_to(folder)),(2026,1,1,0,0,0));info.compress_type=zipfile.ZIP_DEFLATED;zip.writestr(info,p.read_bytes())
 for name in ['package.json','package-lock.json']:zip.writestr('node/'+name,(recipe/name).read_bytes())
 pip=next((prefix/'lib/python3.14/ensurepip/_bundled').glob('pip-*.whl'))
 with zipfile.ZipFile(pip) as wheel:
  for name in wheel.namelist():
   if not name.endswith('/'):zip.writestr('python/lib/python3.14/site-packages/'+name,wheel.read(name))
   if any(word in Path(name).name.lower() for word in ['license','copying','notice','authors']):
    target=licenses/'pip'/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(wheel.read(name))
 zip.writestr('extensions.json',json.dumps(extensions,sort_keys=True))
os.replace(bundle,assets/'runtime.zip');files['assets/agent-runtime/runtime.zip']=hashlib.sha256((assets/'runtime.zip').read_bytes()).hexdigest()
(assets/'runtime.json').write_text(json.dumps({'version':sys.argv[3],'archive_sha256':files['assets/agent-runtime/runtime.zip']},indent=2)+'\n')
files['assets/agent-runtime/runtime.json']=hashlib.sha256((assets/'runtime.json').read_bytes()).hexdigest()
notices=json.loads((recipe/'python-notices/SHA256.json').read_text())
for name,digest in notices.items():
 p=recipe/'python-notices'/name;assert hashlib.sha256(p.read_bytes()).hexdigest()==digest;target=licenses/'python'/name;target.parent.mkdir(exist_ok=True);shutil.copyfile(p,target)
shutil.copyfile(recipe/'python-notices/COMPONENTS.json',licenses/'python/COMPONENTS.json')
shutil.copyfile(source/'LICENSE',licenses/'Node-LICENSE.txt');shutil.copyfile(ndk/'sources/android/cpufeatures/NOTICE',licenses/'NDK-cpufeatures-NOTICE.txt');shutil.copyfile(recipe/'node-android-build.patch',licenses/'node-android-build.patch')
for label,folder in [('npm',source/'deps/npm'),('react',work/'react/node_modules')]:
 for p in folder.rglob('*'):
  if p.is_file() and not p.is_symlink() and p.name.lower().startswith(('license','notice','copying','copyright')):
   target=licenses/label/p.relative_to(folder);target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,target)
frontend=json.loads((recipe/'frontend-notices/SHA256.json').read_text())
for name,digest in frontend.items():
 p=recipe/'frontend-notices'/name;assert hashlib.sha256(p.read_bytes()).hexdigest()==digest
 target=licenses/'frontend'/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,target)
(licenses/'SOURCE.txt').write_text('Python3.14.10 Android publisher package https://www.python.org/ftp/python/3.14.10/python-3.14.10-aarch64-linux-android.tar.gz SHA256423c80b957330cf0ebb92b9b20f2294c292144a441bdb0ceb8c69d14928e569b. Full source-derived dependency inventory and incorporated notices: python/COMPONENTS.json and python/. Pip26.2.2 is the unchanged pure wheel bundled by that CPython package; all root/vendor notices are included under pip/.\nNode22.23.4 source https://nodejs.org/dist/v22.23.4/node-v22.23.4.tar.xz SHA2567b9adc82d612d0a2a863d96c46de2292552e250cee51956ebbe0146c42aacae9; npm10.9.9 comes from that source. Android-only source build metadata patch is included; source and runtime ISA/security features are otherwise unchanged. NDK27.2 CPU detection source has its bundled notice.\nReact19.1.1, ReactDOM19.1.1, Vite6.4.1, RollupWASM4.68.0 and esbuild0.25.12 use exact npm integrity pins in package-lock.json. Native esbuild is built from pinned commit208f539945b145e7c9d6d844290f81c3fe5af320 with Go1.23.12 and the included Android-only directory error normalization patch. It retains OS file permissions and handles an existing unlistable Android ancestor through esbuild’s existing EACCES traversal path. Package notices are retained under react/; compiled Rust/WASM and Go source inventories/notices are under frontend/.\nAll native programs/extensions are immutable APK libraries, not executable downloads in shared storage. Standard libraries/packages are unpacked for normal module loading. addons/assistant/scripts/android/build-agent-runtime.sh reproduces this cohort.\n')
shutil.copyfile(recipe/'package-lock.json',licenses/'package-lock.json')
license_files={str(p.relative_to(licenses)):hashlib.sha256(p.read_bytes()).hexdigest() for p in licenses.rglob('*') if p.is_file()}
manifest={'inputs':sys.argv[3],'python':'3.14.10','node':'22.23.4','react':'19.1.1','vite':'6.4.1','rollup_wasm':'4.68.0','esbuild':esbuild['version'],'esbuild_source_commit':commit,'esbuild_patch_sha256':hashlib.sha256((recipe/'esbuild-android-directory.patch').read_bytes()).hexdigest(),'files':files,'public_workspace_verified':False,'licenses_complete':True,'license_files':license_files}
(out/'manifest.json.new').write_text(json.dumps(manifest,indent=2)+'\n');os.replace(out/'manifest.json.new',out/'manifest.json')
print('Android Python/Node cohort built; production adapter/public-workspace proof remain separate.')
PY
