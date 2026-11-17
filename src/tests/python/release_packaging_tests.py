#!/usr/bin/env python3
"""Release staging never includes workstation state or private signing material."""
import argparse
import importlib.util
import os
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest import mock


SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPTS))
from environment import load_environment

spec = importlib.util.spec_from_file_location("release_packager", SCRIPTS / "package-release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.base / "repository"
        self.root.mkdir()
        self.write("src/CMakeLists.txt", "project(Pilon VERSION 1.0.0 LANGUAGES C CXX)\n")
        self.write("COPYING", "Application license\n")
        self.write("src/scripts/build.sh", "#!/bin/sh\n")
        self.write("addons/assistant/vendor/breeze/models/model.py", "# Source, not a downloaded model\n")

    def write(self, name, value):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value)
        return path

    def args(self, kind="source"):
        return argparse.Namespace(kind=kind, output_dir=self.base / "release", linux_binary=self.root / "builds/pilon", android_apk=None, apksigner=Path("apksigner"))

    def test_source_excludes_private_and_generated_files(self):
        for name in (".env", "tasks/private.md", "builds/AgentFiles/artifact.txt", "runtime/auth.json", "models/model.gguf", "src/.env.private", "src/auth.json", "src/private.keystore", "src/local.key", "src/cache.sqlite", "src/__pycache__/worker.pyc", "src/build/secret.txt", "src/file.backup-old", "addons/calendar/server/data/private.txt", "addons/calendar/server/frontend/dist/generated.js"):
            self.write(name, "PRIVATE")
        self.write(".env.example", "PILON_EXAMPLE=\n")
        self.write("README.md", "# Pilon\n")
        self.write(".github/hooks/commit-msg", "#!/bin/sh\n")
        archive, checksums = release.package(self.args(), self.root)
        self.assertEqual(archive.name, "pilon-1.0.0-source.tar.gz")
        with tarfile.open(archive) as file:
            names = file.getnames()
            self.assertTrue(any(name.endswith("/vendor/breeze/models/model.py") for name in names))
            self.assertTrue(any(name.endswith("/.env.example") for name in names))
            self.assertTrue(any(name.endswith("/README.md") for name in names))
            self.assertTrue(any(name.endswith("/.github/hooks/commit-msg") for name in names))
            self.assertTrue(all(b"PRIVATE" not in file.extractfile(member).read() for member in file if member.isfile()))
        self.assertEqual(checksums.read_text(), release.digest(archive) + "  " + archive.name + "\n")

    def test_source_rejects_symlink_and_key_contents(self):
        private = self.base / "private"
        private.write_text("private")
        link = self.root / "src/link.txt"
        link.symlink_to(private)
        with self.assertRaisesRegex(ValueError, "symbolic links"):
            list(release.source_files(self.root))
        link.unlink()
        for label in ("PRIVATE KEY", "DSA PRIVATE KEY", "PGP PRIVATE KEY BLOCK"):
            with self.subTest(label=label):
                self.write("src/disguised.txt", "-----BEGIN " + label + "-----\n")
                with self.assertRaisesRegex(ValueError, "Private key material"):
                    list(release.source_files(self.root))

    def test_archive_is_reproducible(self):
        first = release.package(self.args(), self.root)[0].read_bytes()
        os.utime(self.root / "COPYING", (123456789, 123456789))
        second = release.package(self.args(), self.root)[0].read_bytes()
        self.assertEqual(first, second)

    def test_linux_stages_only_selected_outputs(self):
        binary = self.write("builds/pilon", "executable")
        binary.chmod(0o755)
        for addon in release.ADDONS:
            self.write("builds/addons/" + addon + ".piext", "compiled addon")
        self.write("builds/AgentFiles/artifacts/private.txt", "PRIVATE")
        self.write("builds/qt.conf", "PRIVATE")
        self.write("runtime/codex/auth.json", "PRIVATE")
        runtime = self.write("runtime/bin/llama-server", "native worker")
        runtime.chmod(0o755)
        archive = release.package(self.args("linux"), self.root)[0]
        with tarfile.open(archive) as file:
            names = file.getnames()
            self.assertTrue(any(name.endswith("/builds/pilon") for name in names))
            self.assertTrue(any(name.endswith("/runtime/bin/llama-server") for name in names))
            self.assertFalse(any("AgentFiles" in name or "qt.conf" in name or "runtime/codex" in name for name in names))

    def test_android_rejects_debug_certificate(self):
        result = mock.Mock(stdout="Signer #1 certificate DN: CN=Android Debug,O=Android,C=US", stderr="")
        with mock.patch.object(release.subprocess, "run", return_value=result):
            with self.assertRaisesRegex(ValueError, "development signing"):
                release.verify_android(Path("app.apk"), Path("apksigner"))

    def test_android_verifier_uses_configured_or_default_jdk(self):
        for configured, expected in ((None, "/home/release-tester/Android/jdk-21"), ("/opt/jdk", "/opt/jdk")):
            with self.subTest(configured=configured):
                environment = {"PATH": "/tool/bin"}
                if configured:
                    environment["JAVA_HOME"] = configured
                result = mock.Mock(stdout="Signer certificate DN: CN=Pilon Release", stderr="")
                with mock.patch.dict(os.environ, environment, clear=True), mock.patch.object(Path, "home", return_value=Path("/home/release-tester")), mock.patch.object(release.subprocess, "run", return_value=result) as run:
                    release.verify_android(Path("app.apk"), Path("apksigner"))
                child = run.call_args.kwargs["env"]
                self.assertEqual(child["JAVA_HOME"], expected)
                self.assertEqual(child["PATH"], expected + "/bin" + os.pathsep + "/tool/bin")

    def test_environment_precedence_and_private_permissions(self):
        private = self.base / "release.env"
        private.write_text("PILON_SETTING=release\nPILON_INHERITED=release\n")
        private.chmod(0o600)
        self.write(".env", f"PILON_RELEASE_ENV='{private}'\nPILON_SETTING=project\nPILON_INHERITED=project\n")
        with mock.patch.dict(os.environ, {"PILON_INHERITED": "process"}, clear=True):
            load_environment(self.root)
            self.assertEqual(os.environ["PILON_SETTING"], "release")
            self.assertEqual(os.environ["PILON_INHERITED"], "process")
        private.chmod(0o644)
        with mock.patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(ValueError, "mode 0600"):
                load_environment(self.root)

    def test_environment_does_not_execute_shell(self):
        marker = self.base / "executed"
        self.write(".env", f"PILON_VALUE='$(touch {marker})'\n")
        with mock.patch.dict(os.environ, {}, clear=True):
            load_environment(self.root)
            self.assertEqual(os.environ["PILON_VALUE"], f"$(touch {marker})")
        self.assertFalse(marker.exists())

    def test_corresponding_source_checks_exact_cached_digest(self):
        source = self.write("build/cached-upstream.tar.gz", "pinned source")
        app = self.write("build/app-source.tar.gz", "application source")
        out = self.base / "release"
        out.mkdir()
        catalog = (("upstream.tar.gz", release.digest(source), "https://example.invalid/source", ("build/cached-upstream.tar.gz",)),)
        with mock.patch.object(release, "SOURCE_ARCHIVES", catalog):
            files = release.corresponding_sources(self.root, out, "pilon-test", "source", app)
            self.assertEqual(len(files), 2)
            self.assertEqual(files[0].read_text(), "pinned source")
            source.write_text("modified source")
            release.corresponding_sources(self.root, out, "pilon-test", "source", app)
            self.assertIn("does not match", files[-1].read_text())


if __name__ == "__main__":
    unittest.main()
