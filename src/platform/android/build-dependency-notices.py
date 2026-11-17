#!/usr/bin/env python3
"""Package notices and audit the exact Gradle production dependency graph."""
import hashlib
import io
import json
from pathlib import Path
import shutil
import struct
import sys
import xml.etree.ElementTree as ET
import zipfile


def elf_alignment(data):
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('Expected a little-endian ELF64 library')
    offset = struct.unpack_from('<Q', data, 32)[0]
    size, count = struct.unpack_from('<HH', data, 54)
    loads = [struct.unpack_from('<Q', data, offset + i * size + 48)[0]
             for i in range(count) if struct.unpack_from('<I', data, offset + i * size)[0] == 1]
    if not loads or min(loads) < 16384:
        raise ValueError('Android native dependency is not 16 KB aligned')
    return loads


def archive_notices(data, destination, native, prefix=''):
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for entry in archive.infolist():
            name = Path(entry.filename).name
            if entry.is_dir():
                continue
            if name.lower().startswith(('license', 'notice', 'copyright', 'copying')):
                target = destination / (prefix + entry.filename.replace('/', '_'))
                target.write_bytes(archive.read(entry))
            elif entry.filename == 'classes.jar':
                archive_notices(archive.read(entry), destination, native, 'classes_')
            elif entry.filename.startswith('jni/arm64-v8a/') and name.endswith('.so'):
                contents = archive.read(entry)
                native.append({'library': name, 'sha256': hashlib.sha256(contents).hexdigest(),
                               'load_alignment': elf_alignment(contents)})


def generate(rows, output):
    sources = Path(__file__).with_name('dependency-licenses')
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    for entry in json.loads((sources / 'SOURCES.json').read_text()):
        source = sources / entry['file']
        if hashlib.sha256(source.read_bytes()).hexdigest() != entry['sha256']:
            raise ValueError('Dependency license source changed: ' + str(source))
        shutil.copyfile(source, output / entry['file'])
    shutil.copyfile(sources / 'SOURCES.json', output / 'SOURCES.json')
    manifest = []
    for row in sorted({r['artifact']: r for r in rows}.values(), key=lambda x: (x['group'], x['name'], x['version'])):
        artifact = Path(row['artifact'])
        identity = ':'.join(row[k] for k in ('group', 'name', 'version'))
        folder = output / identity.replace(':', '_')
        folder.mkdir()
        pom = ET.parse(row['pom']).getroot()
        ns = {'m': 'http://maven.apache.org/POM/4.0.0'}
        licenses = [{child.tag.split('}')[-1]: child.text for child in item}
                    for item in pom.findall('m:licenses/m:license', ns)]
        native = []
        archive_notices(artifact.read_bytes(), folder, native)
        names = ' '.join(str(item) for item in licenses).lower()
        texts = sorted(p.name for p in folder.iterdir())
        if 'apache' in names or row['name'] in ('httpcore5', 'listenablefuture'):
            texts.append('../Apache-2.0.txt')
        elif row['group'].startswith('com.microsoft.identity') and 'mit' in names:
            texts.append('../Microsoft-MIT.txt')
        elif 'android software development kit' in names or 'developer.android.com/studio/terms' in names:
            texts.append('../Google-Android-SDK-Terms.html')
        elif not texts:
            raise ValueError('No complete license text for ' + identity + ': ' + names)
        shutil.copyfile(row['pom'], folder / 'artifact.pom')
        manifest.append({'module': identity, 'sha256': hashlib.sha256(artifact.read_bytes()).hexdigest(),
                         'licenses': licenses, 'license_files': texts, 'native_libraries': native})
    (output / 'DEPENDENCIES.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Packaged {len(manifest)} production dependency notices; '
          f'audited {sum(len(r["native_libraries"]) for r in manifest)} ARM64 libraries.')


def self_test():
    header = bytearray(128)
    header[:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<Q', header, 32, 64)
    struct.pack_into('<HH', header, 54, 56, 1)
    struct.pack_into('<I', header, 64, 1)
    struct.pack_into('<Q', header, 112, 16384)
    assert elf_alignment(header) == [16384]
    struct.pack_into('<Q', header, 112, 4096)
    try:
        elf_alignment(header)
    except ValueError:
        pass
    else:
        raise AssertionError('4 KB ELF was accepted')
    print('Dependency native alignment self-test passed.')


if __name__ == '__main__':
    if sys.argv[1:] == ['--self-test']:
        self_test()
    else:
        generate(json.loads(Path(sys.argv[1]).read_text()), Path(sys.argv[2]))
