#!/usr/bin/env python3
"""Read-only proof that an APK contains the coherent Qt Android surface backport."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zipfile


def dex_classes(data):
    assert data[:4] == b'dex\n', 'Unexpected Android bytecode format'
    string_count, string_offset, type_count, type_offset = struct.unpack_from('<IIII', data, 56)
    class_count, class_offset = struct.unpack_from('<II', data, 96)
    for index in range(class_count):
        class_id = struct.unpack_from('<I', data, class_offset + 32 * index)[0]
        assert class_id < type_count
        descriptor_id = struct.unpack_from('<I', data, type_offset + 4 * class_id)[0]
        assert descriptor_id < string_count
        offset = struct.unpack_from('<I', data, string_offset + 4 * descriptor_id)[0]
        while data[offset] & 0x80:
            offset += 1
        offset += 1
        yield data[offset:data.index(0, offset)].decode('utf-8')


def verify(apk, artifacts=None):
    root = Path(__file__).resolve().parents[4]
    artifacts = artifacts or root / 'builds/android/qt-platform'
    manifest = json.loads((artifacts / 'manifest.json').read_text())
    name = 'libplugins_platforms_qtforandroid_arm64-v8a.so'
    source = artifacts / 'plugins' / name
    assert hashlib.sha256(source.read_bytes()).hexdigest() == manifest['files']['plugins/' + name]
    ndk = Path(os.environ.get('ANDROID_NDK_ROOT', str(Path.home() / 'Android/Sdk/ndk/27.2.12479018')))
    with tempfile.TemporaryDirectory(prefix='pilon-qt-apk-proof-') as directory:
        stripped = Path(directory) / name
        subprocess.run([str(ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip'),
                        '--strip-unneeded', '-o', str(stripped), str(source)], check=True)
        with zipfile.ZipFile(apk) as archive:
            entries = [n for n in archive.namelist() if n.endswith('/' + name)]
            assert len(entries) == 1, 'APK must contain exactly one Android platform plugin'
            assert archive.read(entries[0]) == stripped.read_bytes(), 'APK contains the wrong Qt platform plugin'
            dex = [archive.read(n) for n in archive.namelist() if n.endswith('.dex')]
            definitions = Counter(name for data in dex for name in dex_classes(data))
            expected = ['Lorg/qtproject/qt/android/' + name + ';'
                        for name in ('QtSurface', 'QtSurfaceInterface', 'QtTextureView', 'QtWindow', 'QtInputConnection')]
            assert all(definitions[name] == 1 for name in expected), 'Qt surface class is missing or duplicated'
            assert b'm_pendingSurfaceReleases' in b''.join(dex), 'Patched QtWindow implementation is missing'
    kit = Path(os.environ.get('PILON_ANDROID_QT', str(Path.home() / 'Qt/6.12.0/android_arm64_v8a')))
    prefixes = ['org/qtproject/qt/android/' + n for n in ('QtSurface', 'QtSurfaceInterface', 'QtTextureView', 'QtWindow', 'QtInputConnection')]
    def replaced(name):
        return any(name == prefix + '.class' or name.startswith(prefix + '$') for prefix in prefixes)
    with zipfile.ZipFile(kit / 'jar/Qt6Android.jar') as original, zipfile.ZipFile(artifacts / 'pilon-Qt6Android.jar') as patched:
        unchanged = [n for n in original.namelist() if not replaced(n)]
        assert all(original.read(n) == patched.read(n) for n in unchanged), 'Unrelated Qt Java class changed'
        assert all(n in original.namelist() or replaced(n) for n in patched.namelist()), 'Unexpected Java class was added'
        assert len(patched.namelist()) == len(set(patched.namelist())), 'Duplicate jar entry'
        if 'ime_options_patch_sha256' in manifest:
            name = 'org/qtproject/qt/android/QtInputConnection.class'
            assert patched.read(name) != original.read(name), 'Patched Qt input connection is missing'
    return {'apk': str(apk.resolve()), 'apk_sha256': hashlib.sha256(apk.read_bytes()).hexdigest(),
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
