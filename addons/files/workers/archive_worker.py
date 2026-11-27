import io
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import signal
import stat
import sys
import tarfile
import tempfile
import zipfile

MAX_EXTRACT_ENTRIES = 10_000
MAX_EXTRACT_BYTES = 8 * 1024 ** 3
MAX_ZIP_INDEX_BYTES = 8 * 1024 ** 2
COPY_CHUNK_BYTES = 1024 ** 2


class BoundedZipInput(io.BufferedReader):
    def read(self, size=-1):
        # ZipFile reads the central directory before returning its entry list.
        if size < 0:
            size = max(0, os.fstat(self.fileno()).st_size - self.tell())
        if size > MAX_ZIP_INDEX_BYTES:
            raise ValueError('Archive exceeds the ZIP metadata size limit.')
        return super().read(size)


def extract(source, stage):
    outputs = set()
    entries = 0
    remaining = MAX_EXTRACT_BYTES

    def member(name):
        nonlocal entries
        entries += 1
        if entries > MAX_EXTRACT_ENTRIES:
            raise ValueError('Archive exceeds the extraction entry limit.')
        path = PurePosixPath(name)
        if not name or path.is_absolute() or '..' in path.parts or '\\' in name or '\x00' in name:
            raise ValueError('Archive contains an unsafe path: ' + name)
        output = stage.joinpath(*path.parts)
        # Also count directories implied by nested names.
        parent = output
        while parent != stage and parent not in outputs:
            if len(outputs) >= MAX_EXTRACT_ENTRIES:
                raise ValueError('Archive exceeds the extraction entry limit.')
            outputs.add(parent)
            parent = parent.parent
        return output

    def write(output, is_directory, size, permissions, open_input):
        nonlocal remaining
        if is_directory:
            if size != 0:
                raise ValueError('Archive directory has unexpected contents.')
            output.mkdir(parents=True, exist_ok=True)
            return
        if size < 0 or size > remaining:
            raise ValueError('Archive exceeds the extracted size limit.')
        output.parent.mkdir(parents=True, exist_ok=True)
        written = 0
        with open_input() as inp, output.open('xb') as out:
            while chunk := inp.read(min(COPY_CHUNK_BYTES, remaining + 1)):
                remaining -= len(chunk)
                if remaining < 0:
                    raise ValueError('Archive exceeds the extracted size limit.')
                written += len(chunk)
                out.write(chunk)
        if written != size:
            raise ValueError('Archive entry did not match its recorded size.')
        if permissions is not None:
            output.chmod(permissions)

    if zipfile.is_zipfile(source):
        with BoundedZipInput(io.FileIO(source)) as stream, zipfile.ZipFile(stream) as archive:
            items = archive.infolist()
            if len(items) > MAX_EXTRACT_ENTRIES:
                raise ValueError('Archive exceeds the extraction entry limit.')
            for item in items:
                output = member(item.orig_filename)
                mode = item.external_attr >> 16
                if stat.S_IFMT(mode) and not stat.S_ISREG(mode) and not stat.S_ISDIR(mode):
                    raise ValueError('Archive contains links or special files.')
                write(output, item.is_dir(), item.file_size, mode & 0o777 or None,
                      lambda: archive.open(item))
    else:
        with tarfile.open(source, 'r|*') as archive:
            for item in archive:
                output = member(item.name)
                if not item.isdir() and not item.isfile():
                    raise ValueError('Archive contains links or special files.')
                write(output, item.isdir(), item.size, item.mode & 0o777,
                      lambda: archive.extractfile(item))


def publish_directory(stage, target):
    target.mkdir()
    reserved = target.lstat()
    try:
        stage.chmod(stat.S_IMODE(reserved.st_mode))
        # Publish all entries at once. A populated destination blocks replacement.
        os.replace(stage, target)
    except BaseException:
        try:
            if os.path.samestat(reserved, target.lstat()):
                target.rmdir()
        except OSError:
            pass
        raise


def run(mode, source, target):
    source, target = Path(source), Path(target)
    if not source.is_absolute() or not target.is_absolute() or source.is_symlink():
        raise ValueError('Select an absolute path that is not a symbolic link.')
    if os.path.lexists(target):
        raise ValueError('The destination already exists. Rename it or choose another destination.')
    stage = Path(tempfile.mkdtemp(prefix='.pilon-archive-', dir=target.parent))
    try:
        if mode == 'archive':
            if not source.is_dir(): raise ValueError('Select a directory to archive.')
            if target.parent.resolve().is_relative_to(source.resolve()): raise ValueError('The archive must be outside the source directory.')
            result = stage / 'output.zip'
            with zipfile.ZipFile(result, 'w', zipfile.ZIP_DEFLATED, compresslevel=1, allowZip64=True, strict_timestamps=False) as archive:
                for root, dirs, files in os.walk(source, followlinks=False):
                    for name in dirs + files:
                        item = Path(root) / name
                        if item.is_symlink(): raise ValueError('Cannot archive symbolic links: ' + str(item))
                    archive.write(root, Path(root).relative_to(source.parent).as_posix() + '/')
                    for name in files:
                        item = Path(root) / name
                        if not item.is_file(): raise ValueError('Cannot archive special files: ' + str(item))
                        archive.write(item, item.relative_to(source.parent).as_posix())
            # A hard link publishes without replacing a file created concurrently.
            os.link(result, target)
        else:
            extract(source, stage)
            publish_directory(stage, target)
            stage = None
    finally:
        if stage is not None:
            shutil.rmtree(stage, ignore_errors=True)


def main():
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    signal.signal(signal.SIGHUP, lambda *_: sys.exit(1))
    if len(sys.argv) > 4 and sys.argv[4] == 'watch':
        import threading
        def disconnected():
            while os.read(0, 1): pass
            os.kill(os.getpid(), signal.SIGTERM)
        threading.Thread(target=disconnected, daemon=True).start()
    try:
        run(*sys.argv[1:4])
        print(json.dumps({'status':'ok'}))
    except Exception as error:
        print(json.dumps({'status':'error', 'error':str(error)}))


if __name__ == '__main__':
    main()
