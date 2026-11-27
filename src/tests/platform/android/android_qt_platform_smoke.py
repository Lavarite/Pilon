#!/usr/bin/env python3
"""Read-only proof that an APK contains the coherent Qt Android surface backport."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'src/scripts'))
from environment import development_root

IME_CLASS = 'Lorg/qtproject/qt/android/QtInputConnection;'


def require(valid, message):
    if not valid:
        raise ValueError(message)


def dex_classes(data):
    require(len(data) >= 112 and data[:4] == b'dex\n', 'Unexpected Android bytecode format')
    string_count, string_offset, type_count, type_offset = struct.unpack_from('<IIII', data, 56)
    class_count, class_offset = struct.unpack_from('<II', data, 96)
    for offset, count, size in ((string_offset, string_count, 4), (type_offset, type_count, 4), (class_offset, class_count, 32)):
        require(offset + count * size <= len(data), 'Invalid Android bytecode table')
    for index in range(class_count):
        class_id = struct.unpack_from('<I', data, class_offset + 32 * index)[0]
        require(class_id < type_count, 'Invalid class descriptor type')
        descriptor_id = struct.unpack_from('<I', data, type_offset + 4 * class_id)[0]
        require(descriptor_id < string_count, 'Invalid class descriptor string')
        offset = struct.unpack_from('<I', data, string_offset + 4 * descriptor_id)[0]
        while offset < len(data) and data[offset] & 0x80:
            offset += 1
        require(offset + 1 < len(data), 'Truncated class descriptor')
        offset += 1
        yield data[offset:data.index(0, offset)].decode('utf-8')


def patched_ime_dispatch(lines):
    inside_class = inside_method = False
    checks = set()
    for line in lines:
        field, _, value = line.partition(':')
        if field.strip() == 'Class descriptor':
            inside_class = value.strip().strip("'") == IME_CLASS
            inside_method = False
        elif inside_class and field.strip() == 'name':
            inside_method = value.strip() == "'sendKeyEvent'"
            checks.clear()
        if not inside_method:
            continue
        if '.m_imeOptions:I' in line and 'iget' in line:
            checks = {'options'}
        if 'options' not in checks:
            continue
        if re.search(r'\bconst(?:/high16)?\b.*#int 1073741824\b', line):
            checks.add('no-enter')
        if re.search(r'\band-int\s', line):
            checks.add('flag-mask')
        if re.search(r'\band-int/lit16\b.*#int 255\b', line):
            checks.add('action-mask')
        if ('sparse-switch ' in line or 'packed-switch ' in line) and len(checks) == 4:
            return True
    return False


def verify_ime_dispatch(dex, directory):
    sdk = Path(os.environ.get('ANDROID_SDK_ROOT') or os.environ.get('ANDROID_HOME') or Path.home() / 'Android/Sdk')
    dexdump = sdk / 'build-tools/37.0.0/dexdump'
    source = Path(directory) / 'qt-input.dex'
    source.write_bytes(dex)
    with subprocess.Popen([dexdump, '-d', source], stdout=subprocess.PIPE, text=True, encoding='utf-8', errors='replace') as process:
        patched = patched_ime_dispatch(process.stdout)
        # Consume the remaining disassembly so dexdump can finish normally.
        for _ in process.stdout:
            pass
        require(process.wait() == 0, 'Unable to disassemble Qt input DEX')
    require(patched, 'APK contains the wrong QtInputConnection Enter-key dispatch')


def verify(apk, artifacts=None):
    artifacts = artifacts or development_root() / 'builds/android/qt-platform'
    manifest = json.loads((artifacts / 'manifest.json').read_text())
    name = 'libplugins_platforms_qtforandroid_arm64-v8a.so'
    source = artifacts / 'plugins' / name
    require(hashlib.sha256(source.read_bytes()).hexdigest() == manifest['files']['plugins/' + name],
            'Qt platform plugin does not match its manifest')
    ndk = Path(os.environ.get('ANDROID_NDK_ROOT', str(Path.home() / 'Android/Sdk/ndk/27.2.12479018')))
    with tempfile.TemporaryDirectory(prefix='pilon-qt-apk-proof-') as directory:
        stripped = Path(directory) / name
        subprocess.run([str(ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip'),
                        '--strip-unneeded', '-o', str(stripped), str(source)], check=True)
        with zipfile.ZipFile(apk) as archive:
            entries = [n for n in archive.namelist() if n.endswith('/' + name)]
            require(len(entries) == 1, 'APK must contain exactly one Android platform plugin')
            require(archive.read(entries[0]) == stripped.read_bytes(), 'APK contains the wrong Qt platform plugin')
            definitions = Counter()
            ime_dex = None
            surface_marker = False
            for entry in archive.namelist():
                if not entry.endswith('.dex'):
                    continue
                data = archive.read(entry)
                classes = Counter(dex_classes(data))
                definitions.update(classes)
                if classes[IME_CLASS]:
                    ime_dex = data
                surface_marker |= b'm_pendingSurfaceReleases' in data
            expected = ['Lorg/qtproject/qt/android/' + name + ';'
                        for name in ('QtSurface', 'QtSurfaceInterface', 'QtTextureView', 'QtWindow', 'QtInputConnection')]
            require(all(definitions[name] == 1 for name in expected), 'Qt surface class is missing or duplicated')
            require(surface_marker, 'Patched QtWindow implementation is missing')
            if 'ime_options_patch_sha256' in manifest:
                verify_ime_dispatch(ime_dex, directory)
    kit = Path(os.environ.get('PILON_ANDROID_QT', str(Path.home() / 'Qt/6.12.0/android_arm64_v8a')))
    prefixes = ['org/qtproject/qt/android/' + n for n in ('QtSurface', 'QtSurfaceInterface', 'QtTextureView', 'QtWindow', 'QtInputConnection')]
    def replaced(name):
        return any(name == prefix + '.class' or name.startswith(prefix + '$') for prefix in prefixes)
    staged_jar = artifacts / 'pilon-Qt6Android.jar'
    require(hashlib.sha256(staged_jar.read_bytes()).hexdigest() == manifest['files'][staged_jar.name],
            'Staged Qt Java archive does not match its manifest')
    with zipfile.ZipFile(kit / 'jar/Qt6Android.jar') as original, zipfile.ZipFile(staged_jar) as patched:
        unchanged = [n for n in original.namelist() if not replaced(n)]
        require(all(original.read(n) == patched.read(n) for n in unchanged), 'Unrelated Qt Java class changed')
        require(all(n in original.namelist() or replaced(n) for n in patched.namelist()), 'Unexpected Java class was added')
        require(len(patched.namelist()) == len(set(patched.namelist())), 'Duplicate jar entry')
        if 'ime_options_patch_sha256' in manifest:
            name = 'org/qtproject/qt/android/QtInputConnection.class'
            require(patched.read(name) != original.read(name), 'Patched Qt input connection is missing')
    with apk.open('rb') as apk_file:
        apk_digest = hashlib.file_digest(apk_file, 'sha256').hexdigest()
    return {'apk': str(apk.resolve()), 'apk_sha256': apk_digest,
            'platform_plugin_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
            'platform_plugin_count': len(entries), 'surface_class_definitions': {name: definitions[name] for name in expected},
            'unchanged_jar_entries': len(unchanged), 'patch_sha256': manifest['patch_sha256'],
            'ime_options_patch_sha256': manifest.get('ime_options_patch_sha256'),
            'scope': 'Packaged native/Java coherence; device behavior is a separate check.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('apk', type=Path)
    parser.add_argument('--artifacts', type=Path)
    arguments = parser.parse_args()
    print(json.dumps(verify(arguments.apk, arguments.artifacts), indent=2))
