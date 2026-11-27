#!/usr/bin/env python3
"""Release staging never includes workstation state or private signing material."""
import argparse
import importlib.util
import os
from pathlib import Path
import json
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
from zipfile import ZipFile


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
        self.approve("src/CMakeLists.txt", "COPYING", "src/scripts/build.sh", "addons/assistant/vendor/breeze/models/model.py")

    def approve(self, *paths):
        manifest = self.root / "SOURCE-FILES.txt"
        with manifest.open("a", encoding="utf-8") as file:
            file.writelines(path + "\n" for path in paths)

    def android_fixture(self, notices=None):
        args = self.args("android")
        args.android_apk = self.root / "app.apk"
        with ZipFile(args.android_apk, "w") as apk:
            for name, contents in (notices or {}).items():
                apk.writestr("assets/licenses/android-dependencies/" + name, contents)
        return args

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
        self.approve(".env.example", "README.md", ".github/hooks/commit-msg", "src/auth.json")
        self.write("src/unfinished-notes.txt", "PRIVATE")
        self.write("addons/notes/local-export.json", "PRIVATE")
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
        self.approve("src/link.txt")
        with self.assertRaisesRegex(ValueError, "symbolic links"):
            list(release.source_files(self.root))
        link.unlink()
        self.write("src/link.txt", "ordinary source")
        self.approve("src/disguised.txt")
        for label in ("PRIVATE KEY", "DSA PRIVATE KEY", "PGP PRIVATE KEY BLOCK"):
            with self.subTest(label=label):
                self.write("src/disguised.txt", "-----BEGIN " + label + "-----\n")
                with self.assertRaisesRegex(ValueError, "Private key material"):
                    list(release.source_files(self.root))

    def test_missing_approved_source_fails_before_archive_is_published(self):
        self.approve("addons/files/package/NewControl.qml")
        with self.assertRaisesRegex(ValueError, "NewControl.qml"):
            release.package(self.args(), self.root)
        self.assertFalse((self.base / "release/pilon-1.0.0-source.tar.gz").exists())

    def test_archive_is_reproducible(self):
        first = release.package(self.args(), self.root)[0].read_bytes()
        os.utime(self.root / "COPYING", (123456789, 123456789))
        second = release.package(self.args(), self.root)[0].read_bytes()
        self.assertEqual(first, second)

    def test_failed_source_validation_preserves_previous_release(self):
        archive, checksums = release.package(self.args(), self.root)
        old_archive, old_checksums = archive.read_bytes(), checksums.read_bytes()
        self.write("src/CMakeLists.txt", "project(Pilon VERSION 1.0.0 LANGUAGES C CXX)\n# modified\n")
        args = self.args()
        args.corresponding_source = True
        with mock.patch.object(release, "corresponding_sources", side_effect=ValueError("source validation failed")):
            with self.assertRaisesRegex(ValueError, "source validation failed"):
                release.package(args, self.root)
        self.assertEqual(archive.read_bytes(), old_archive)
        self.assertEqual(checksums.read_bytes(), old_checksums)

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
        self.write("runtime/speech/bin/pilon-breeze", "speech worker").chmod(0o755)
        self.write("runtime/speech/bin/libaudiocpp.so.0", "speech library")
        for name in ("audio.cpp", "ggml", "cJSON", "libyaml", "utf8proc", "sentencepiece", "protobuf-lite"):
            self.write("runtime/speech/licenses/" + name + "-LICENSE", "speech license")
        self.write("runtime/speech/models/private.gguf", "PRIVATE")
        archive = release.package(self.args("linux"), self.root)[0]
        with tarfile.open(archive) as file:
            names = file.getnames()
            self.assertTrue(any(name.endswith("/.bin/pilon") for name in names))
            self.assertTrue(any(name.endswith("/.bin/addons/.install-packages") for name in names))
            self.assertFalse(any("AgentFiles" in name or "qt.conf" in name or "/runtime/" in name or "/.runtime/" in name for name in names))

    def test_android_rejects_debug_certificate(self):
        result = mock.Mock(stdout="Signer #1 certificate DN: CN=Android Debug,O=Android,C=US", stderr="")
        with mock.patch.object(release.subprocess, "run", return_value=result):
            with self.assertRaisesRegex(ValueError, "development signing"):
                release.verify_android(Path("app.apk"), Path("apksigner"))

    def test_android_notices_exclude_stale_component_outputs(self):
        apk_notices = {"DEPENDENCIES.json": '[{"module":"org.example:example:1.0"}]',
                       "org.example_example_1.0/LICENSE.txt": "APK dependency license"}
        args = self.android_fixture(apk_notices)
        for component in ("git", "git-providers", "unused-component"):
            self.write(f"builds/android/licenses/{component}/LICENSE.txt", "stale")
        current = {"runtime/codex/LICENSE.txt", "qt-platform/LICENSE.txt"}
        for relative in current:
            self.write("builds/android/licenses/" + relative, "current")

        with mock.patch.object(release, "verify_android"):
            _, archive, _ = release.package(args, self.root)
        with tarfile.open(archive) as file:
            notices = {name.split("/third-party-notices/", 1)[1]
                       for name in file.getnames() if "/third-party-notices/" in name}
            self.assertEqual(notices, current | {"android-dependencies/" + name for name in apk_notices})
            for name, contents in apk_notices.items():
                member = next(path for path in file.getnames() if path.endswith("/android-dependencies/" + name))
                self.assertEqual(file.extractfile(member).read().decode(), contents)

    def test_android_notices_require_a_current_component_license(self):
        args = self.android_fixture()
        self.write("builds/android/licenses/git-providers/LICENSE.txt", "stale")
        with mock.patch.object(release, "verify_android"):
            with self.assertRaisesRegex(ValueError, "staged third-party notices"):
                release.package(args, self.root)

    def test_android_notices_require_inventory_from_the_apk(self):
        args = self.android_fixture({"LICENSE.txt": "license without an inventory"})
        self.write("builds/android/licenses/runtime/LICENSE.txt", "native license")
        with mock.patch.object(release, "verify_android"):
            with self.assertRaisesRegex(ValueError, "APK is missing its dependency notice inventory"):
                release.package(args, self.root)

    def test_android_notices_reject_paths_outside_the_notice_directory(self):
        args = self.android_fixture({"DEPENDENCIES.json": '[{"module":"org.example:example:1.0"}]',
                                     "../LICENSE.txt": "outside notice directory"})
        self.write("builds/android/licenses/runtime/LICENSE.txt", "native license")
        with mock.patch.object(release, "verify_android"):
            with self.assertRaisesRegex(ValueError, "Invalid APK dependency notice path"):
                release.package(args, self.root)

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

    def linux_source_fixture(self, shared_ical=False):
        build = self.base / "alternate-build"
        build.mkdir()
        source = self.write("main.c", "int main(void) { return 0; }\n")
        link = []
        if shared_ical:
            source.write_text("int ical_fixture(void); int main(void) { return ical_fixture(); }\n")
            library = self.write("ical.c", "int ical_fixture(void) { return 0; }\n")
            shared = build / "libical.so.99"
            subprocess.run(["cc", "-shared", "-fPIC", library, "-Wl,-soname,libical.so.99", "-o", shared], check=True)
            link.append(shared)
        subprocess.run(["cc", source, *link, "-Wl,--build-id", "-o", build / "pilon"], check=True)
        binary = self.root / "builds/pilon"
        binary.parent.mkdir()
        shutil.copy2(build / "pilon", binary)
        for name in release.ADDONS:
            self.write("builds/addons/" + name + ".piext", "compiled addon")
        dependency_dir = self.base / "custom-dependencies"
        (build / "CMakeCache.txt").write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root / 'src'}\n"
            f"FETCHCONTENT_BASE_DIR:PATH={dependency_dir}\nLIBICAL_FOUND:INTERNAL={'1' if shared_ical else ''}\n"
            "LIBICAL_VERSION:INTERNAL=99.0\n")
        args = self.args("linux")
        args.corresponding_source = True
        args.linux_build_dir = build
        catalog = []
        for library in ("libvterm", "libical"):
            name = library + "-1.0.tar.gz"
            content = library + " exact source"
            original = self.write(f"builds/work/android/app/{name}", content if library == "libvterm" else "other source")
            if not shared_ical or library != "libical":
                folder = (dependency_dir / f"{library}-tmp" if library == "libvterm"
                    else dependency_dir / f"{library}-subbuild" / f"{library}-populate-prefix" / "src")
                folder.mkdir(parents=True)
                (folder / name).write_text(content)
            correct = self.write("test-source/" + name, content)
            catalog.append((name, release.digest(correct), "https://example.invalid/" + name, (str(original.relative_to(self.root)),)))
        return args, tuple(catalog)

    def package_linux_report(self, args, catalog):
        with mock.patch.object(release, "SOURCE_ARCHIVES", catalog):
            outputs = release.package(args, self.root)
        report = next(path for path in outputs if path.name.endswith("-corresponding-source-report.json"))
        return json.loads(report.read_text())

    def test_linux_fetched_sources_come_from_the_selected_build(self):
        args, catalog = self.linux_source_fixture()
        report = self.package_linux_report(args, catalog)
        self.assertEqual(report["missing"], [])
        self.assertEqual([item["component"] for item in report["provided"]], ["libvterm-1.0.tar.gz", "libical-1.0.tar.gz"])

    def test_linux_dynamic_system_ical_requires_no_fetched_archive(self):
        args, catalog = self.linux_source_fixture(shared_ical=True)
        report = self.package_linux_report(args, catalog)
        self.assertEqual(report["missing"], [])
        self.assertEqual([item["component"] for item in report["provided"]], ["libvterm-1.0.tar.gz"])
        self.assertEqual(report["external"], [{"component": "libical", "sonames": ["libical.so.99"], "version": "99.0"}])

    def test_linux_rejects_unrelated_build_and_source_override(self):
        args, catalog = self.linux_source_fixture()
        alternate = self.write("alternate.c", "int main(void) { return 17; }\n")
        subprocess.run(["cc", alternate, "-Wl,--build-id", "-o", args.linux_binary], check=True)
        report = self.package_linux_report(args, catalog)
        self.assertEqual(report["provided"], [])
        self.assertIn("does not match", report["missing"][0]["reason"])
        shutil.copy2(args.linux_build_dir / "pilon", args.linux_binary)
        cache = args.linux_build_dir / "CMakeCache.txt"
        with cache.open("a") as stream:
            stream.write("FETCHCONTENT_SOURCE_DIR_LIBICAL:PATH=/local/ical\n")
        report = self.package_linux_report(args, catalog)
        self.assertEqual([item["component"] for item in report["provided"]], ["libvterm-1.0.tar.gz"])
        self.assertIn("override", report["missing"][0]["reason"])


if __name__ == "__main__":
    unittest.main()
