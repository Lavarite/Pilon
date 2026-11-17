#!/usr/bin/env python3
"""Build and audit the same live-editor regression against both Qt patch cohorts."""
from pathlib import Path
import os,shutil,subprocess,json,hashlib
root=Path(__file__).resolve().parents[4];work=root/'build/android/qt-platform';probe=root/'build/android/qt-activation-probe';kit=Path.home()/'Qt/6.12.0/android_arm64_v8a';env=os.environ.copy();env.update(JAVA_HOME=str(Path.home()/'Android/jdk-21'));env['PATH']=env['JAVA_HOME']+'/bin:'+env['PATH']
for name,artifacts in [('red',work/'surface-only-artifacts'),('green',root/'builds/android/qt-platform')]:
 package=work/('activation-'+name+'-package');(package/'libs').mkdir(parents=True,exist_ok=True);(package/'qt-platform-native').mkdir(exist_ok=True)
 shutil.copyfile(root/'src/platform/android/qt-platform/tests/build.gradle',package/'build.gradle');shutil.copyfile(artifacts/'pilon-Qt6Android.jar',package/'libs/pilon-Qt6Android.jar');shutil.copyfile(artifacts/'plugins/libplugins_platforms_qtforandroid_arm64-v8a.so',package/'qt-platform-native/libplugins_platforms_qtforandroid_arm64-v8a.so')
 subprocess.run([str(kit/'bin/qt-cmake'),'-S',str(root/'src/platform/android/qt-platform/tests/accessibility'),'-B',str(probe),'-GNinja','-DCMAKE_BUILD_TYPE=Release','-DQT_HOST_PATH='+str(Path.home()/'Qt/6.12.0/gcc_64'),'-DANDROID_SDK_ROOT='+str(Path.home()/'Android/Sdk'),'-DANDROID_NDK_ROOT='+str(Path.home()/'Android/Sdk/ndk/27.2.12479018'),'-DANDROID_PLATFORM=android-28','-DANDROID_ABI=arm64-v8a','-DPILON_PROBE_PACKAGE_DIR='+str(package)],env=env,check=True)
 subprocess.run(['cmake','--build',str(probe),'--target','apk','--parallel','4'],env=env,check=True)
 apk=work/('accessibility-activation-'+name+'.apk');shutil.copyfile(probe/'android-build/build/outputs/apk/debug/android-build-debug.apk',apk)
 proof=json.loads(subprocess.check_output(['python3',str(root/'src/tests/platform/android/android_qt_platform_smoke.py'),str(apk),'--artifacts',str(artifacts)],env=env,text=True));(work/('activation-'+name+'-packaging.json')).write_text(json.dumps(proof,indent=2)+'\n');print(name,hashlib.sha256(apk.read_bytes()).hexdigest(),flush=True)
