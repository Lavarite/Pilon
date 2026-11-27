#!/usr/bin/env python3
"""Compare the current Whisper helper with guarded Android CPU variants."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "src/scripts"))
from environment import development_root

DEV = development_root()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', default=str(Path.home()/'Android/Sdk/platform-tools/adb'))
    parser.add_argument('--model', default='/storage/emulated/0/Documents/Pilon/Models/whisper-large-v3-turbo/ggml-large-v3-turbo-q8_0.bin')
    parser.add_argument('--variants', type=Path, default=DEV / 'dev-deps/android/media-runtime/whisper-variants-build/bin')
    parser.add_argument('--candidate-first', action='store_true')
    args = parser.parse_args()
    remote = '/data/local/tmp/pilon-whisper-bench-'+uuid.uuid4().hex
    proof = DEV / 'dev-deps/android/media-runtime/benchmark'/Path(remote).name
    proof.mkdir(parents=True)
    adb = [args.adb,'-d']
    active = []

    def command(*values, timeout=60):
        result = subprocess.run(adb+list(values),capture_output=True,text=True,timeout=timeout)
        if result.returncode:
            raise RuntimeError(f'Benchmark command failed: {values}\n{result.stderr}')
        return result.stdout

    def shell(*values, **kwargs):
        return command('shell',shlex.join(map(str,values)),**kwargs)

    results = {'scope':'isolated adb-shell calibration; app-UID acceptance is separate','model':args.model,'threads':8,
               'variantDirectory':str(args.variants),'variantLibraries':{},'cases':[]}
    try:
        original = shell('stat','-c','%s %Y',args.model).strip()
        shell('mkdir','-p',remote+'/variants')
        shell('cp',args.model,remote+'/model.bin')
        sample = DEV / 'dev-deps/android/media-runtime/whisper.cpp-b4938/samples/jfk.wav'
        results['sampleSha256'] = hashlib.sha256(sample.read_bytes()).hexdigest()
        command('push',str(sample),remote+'/speech.wav')
        baseline = DEV / 'builds/android/runtime/arm64-v8a/libpilon_whisper-cli.so'
        results['baselineSha256'] = hashlib.sha256(baseline.read_bytes()).hexdigest()
        command('push',str(baseline),remote+'/baseline')
        for source in args.variants.iterdir():
            if source.suffix == '.so' or source.name == 'whisper-cli':
                command('push',str(source),remote+'/variants/'+source.name)
                results['variantLibraries'][source.name]=hashlib.sha256(source.read_bytes()).hexdigest()
        standard = Path.home()/'Android/Sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so'
        command('push',str(standard),remote+'/variants/libc++_shared.so')
        shell('chmod','700',remote+'/baseline',remote+'/variants/whisper-cli')
        cases=[('baseline',remote+'/baseline'),('variants',remote+'/variants/whisper-cli')]
        if args.candidate_first: cases.reverse()
        for name,helper in cases:
            pidfile = remote+'/'+name+'.pid'
            invocation = ['env','LD_LIBRARY_PATH='+remote+'/variants',helper,
                          '-m',remote+'/model.bin','-f',remote+'/speech.wav','-l','auto','-t','8','-nt','-otxt','-of',remote+'/'+name]
            script = 'echo $$ > '+shlex.quote(pidfile)+'; exec '+shlex.join(invocation)
            print('Starting Whisper '+name,flush=True)
            started = time.monotonic()
            process = subprocess.Popen(adb+['shell','-T',script],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            active.append((process,pidfile,helper))
            try:
                output,errors = process.communicate(timeout=180)
            except subprocess.TimeoutExpired:
                raise RuntimeError('Whisper '+name+' exceeded its calibration deadline')
            (proof/(name+'.log')).write_text(output+errors)
            assert process.returncode == 0, errors[-3000:]
            text = command('exec-out','cat',remote+'/'+name+'.txt').strip()
            assert 'country' in text.lower() and 'ask' in text.lower(),text
            case = {'name':name,'elapsedSeconds':round(time.monotonic()-started,3),'transcript':text,
                    'timings':re.findall(r'whisper_print_timings:.*',errors),
                    'backend':re.findall(r'.*(?:load_backend|CPU|NEON|SME).*',errors)}
            results['cases'].append(case)
            (proof/'result.json').write_text(json.dumps(results,indent=2)+'\n')
            print(name+' '+str(case['elapsedSeconds'])+' seconds',flush=True)
        assert shell('stat','-c','%s %Y',args.model).strip()==original,'The source model changed'
        results['sourceModelUnchanged']=True
        (proof/'result.json').write_text(json.dumps(results,indent=2)+'\n')
        print(proof/'result.json',flush=True)
    finally:
        for process,pidfile,helper in active:
            if process.poll() is None:
                try:
                    pid=shell('cat',pidfile).strip()
                    if pid.isdigit() and shell('readlink','/proc/'+pid+'/exe').strip()==helper:
                        shell('kill','-TERM',pid)
                finally:
                    process.terminate()
                    try:process.wait(timeout=5)
                    except subprocess.TimeoutExpired:process.kill();process.wait()
        shell('rm','-rf',remote)


if __name__ == '__main__':
    main()
