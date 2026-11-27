import io
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'workers'))
import archive_worker


class ArchiveWorkerTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.destination = self.root / 'extracted'

    def archive(self, kind, entries):
        source = self.root / ('source.' + kind)
        if kind == 'zip':
            with zipfile.ZipFile(source, 'w', zipfile.ZIP_DEFLATED) as archive:
                for name, data in entries:
                    archive.writestr(name, data)
        else:
            with tarfile.open(source, 'w:gz') as archive:
                for name, data in entries:
                    entry = tarfile.TarInfo(name)
                    entry.size = len(data)
                    archive.addfile(entry, io.BytesIO(data))
        return source

    def check_unpublished(self, destination):
        self.assertFalse(destination.exists())
        self.assertEqual(list(self.root.glob('.pilon-archive-*')), [])

    def test_cumulative_output_bytes(self):
        for kind in ('zip', 'tar.gz'):
            target = self.destination.with_suffix('.' + kind)
            with self.subTest(kind=kind), patch.object(archive_worker, 'MAX_EXTRACT_BYTES', 7):
                source = self.archive(kind, [('one', b'1234'), ('two', b'5678')])
                with self.assertRaisesRegex(ValueError, 'extracted size limit'):
                    archive_worker.run('extract', source, target)
                self.check_unpublished(target)

    def test_maximum_entry_count(self):
        for kind in ('zip', 'tar.gz'):
            target = self.destination.with_suffix('.' + kind)
            with self.subTest(kind=kind), patch.object(archive_worker, 'MAX_EXTRACT_ENTRIES', 2):
                source = self.archive(kind, [('a', b'1'), ('b', b'2'), ('c', b'3')])
                with self.assertRaisesRegex(ValueError, 'entry limit'):
                    archive_worker.run('extract', source, target)
                self.check_unpublished(target)

    def test_zip_metadata_limit(self):
        with patch.object(archive_worker, 'MAX_ZIP_INDEX_BYTES', 32):
            source = self.archive('zip', [('ordinary-file', b'contents')])
            with self.assertRaisesRegex(ValueError, 'ZIP metadata size limit'):
                archive_worker.run('extract', source, self.destination)
            self.check_unpublished(self.destination)

    def test_exact_limits_and_nested_paths(self):
        for kind in ('zip', 'tar.gz'):
            target = self.root / ('extracted-' + kind)
            with self.subTest(kind=kind), patch.multiple(archive_worker, MAX_EXTRACT_BYTES=4, MAX_EXTRACT_ENTRIES=3):
                source = self.archive(kind, [('one', b'12'), ('folder/two', b'34')])
                archive_worker.run('extract', source, target)
                self.assertEqual((target / 'one').read_bytes(), b'12')
                self.assertEqual((target / 'folder' / 'two').read_bytes(), b'34')

    def test_nested_paths_count_implicit_directories(self):
        with patch.object(archive_worker, 'MAX_EXTRACT_ENTRIES', 2):
            source = self.archive('zip', [('outer/inner/file', b'a')])
            with self.assertRaisesRegex(ValueError, 'entry limit'):
                archive_worker.run('extract', source, self.destination)
            self.check_unpublished(self.destination)

    def test_zip64_file_record(self):
        source = self.root / 'zip64.zip'
        with zipfile.ZipFile(source, 'w') as archive, archive.open('file', 'w', force_zip64=True) as entry:
            entry.write(b'zip64 contents')
        archive_worker.run('extract', source, self.destination)
        self.assertEqual((self.destination / 'file').read_bytes(), b'zip64 contents')

    def test_directory_publication_preserves_concurrent_content(self):
        source = self.archive('zip', [('one', b'archive contents')])
        replace = archive_worker.os.replace

        def populate_target_then_replace(stage, target):
            (target / 'one').write_bytes(b'new user contents')
            replace(stage, target)

        with patch.object(archive_worker.os, 'replace', side_effect=populate_target_then_replace):
            with self.assertRaises(OSError):
                archive_worker.run('extract', source, self.destination)
        self.assertEqual((self.destination / 'one').read_bytes(), b'new user contents')
        self.assertEqual(list(self.root.glob('.pilon-archive-*')), [])


if __name__ == '__main__':
    unittest.main()
