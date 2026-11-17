import importlib.util
import os
from pathlib import Path
import re
import subprocess
import struct
import sys
import tarfile
import tempfile
import unittest
import zipfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[4]
SPEC = importlib.util.spec_from_file_location("android_scan", ROOT / "src/scripts/android-qml-importscanner.py")
scanner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(scanner)


class AndroidPackagingTests(unittest.TestCase):
    def test_scan_includes_relocated_addon_qml_without_server_or_tests(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "src"
            source.mkdir()
            (source / "CMakeLists.txt").write_text("project(Pilon)\n")
            names = ("src/app/Main.qml", "addons/calendar/ui/CalendarPage.qml",
                     "addons/calendar/package/Runtime.qml", "addons/calendar/tests/Test.qml",
                     "addons/calendar/server/frontend/app.js")
            for name in names:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("import QtQuick\n")
            self.assertEqual(set(scanner.application_files(source)),
                             {str(root / name) for name in names[:3]})

    def test_apk_requires_packaged_network_resolver(self):
        spec = importlib.util.spec_from_file_location("runtime_scan", ROOT / "addons/assistant/scripts/android-runtime-smoke.py")
        runtime_scan = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runtime_scan)
        elf = bytearray(120)
        elf[:6] = b"\x7fELF\x02\x01"
        struct.pack_into("<HH", elf, 16, 3, 183)
        struct.pack_into("<Q", elf, 32, 64)
        struct.pack_into("<HH", elf, 54, 56, 1)
        struct.pack_into("<I", elf, 64, 1)
        struct.pack_into("<Q", elf, 112, 16384)
        with tempfile.TemporaryDirectory() as directory:
            apk = Path(directory) / "runtime.apk"
            with zipfile.ZipFile(apk, "w") as archive:
                for helper in ("codex", "llama"):
                    archive.writestr(f"lib/arm64-v8a/libpilon_{helper}.so", elf)
            with self.assertRaisesRegex(ValueError, "libpilon_dns.so"):
                runtime_scan.check_apk(apk)
            with zipfile.ZipFile(apk, "a") as archive:
                archive.writestr("lib/arm64-v8a/libpilon_dns.so", elf)
            runtime_scan.check_apk(apk)

    def test_moved_android_entry_uses_repository_paths_from_another_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            calls = temporary / "calls"
            tools = temporary / "bin"
            tools.mkdir()
            recorder = '#!/bin/sh\nprintf "%s\\0" "$0" "$@" >> "$PILON_PACKAGING_CALLS"\nprintf "\\0" >> "$PILON_PACKAGING_CALLS"\n'
            for name in ("cmake", "ninja", "python3", "perl", "make"):
                command = tools / name
                command.write_text(recorder)
                command.chmod(0o755)
            android_qt, host_qt, sdk, ndk, jdk = (temporary / name for name in ("android-qt", "host-qt", "sdk", "ndk", "jdk"))
            for kit in (android_qt, host_qt):
                qmake = kit / "bin/qmake"
                qmake.parent.mkdir(parents=True)
                qmake.write_text('#!/bin/sh\nprintf "6.12.0\\n"\n')
                qmake.chmod(0o755)
            prerequisites = (android_qt / "bin/qt-cmake", ndk / "build/cmake/android.toolchain.cmake",
                             jdk / "bin/javac", sdk / "platforms/android-37.0/android.jar",
                             sdk / "build-tools/37.0.0/apksigner", sdk / "build-tools/37.0.0/zipalign")
            for file in prerequisites:
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text(recorder)
                file.chmod(0o755)
            home = temporary / "home"
            (home / ".android").mkdir(parents=True)
            (home / ".android/debug.keystore").touch()
            environment = os.environ.copy()
            environment.pop("PILON_ANDROID_BUILD_DIR", None)
            environment.update(HOME=str(home), JAVA_HOME=str(jdk), PILON_ANDROID_QT=str(android_qt),
                               QT_HOST_PATH=str(host_qt), ANDROID_SDK_ROOT=str(sdk), ANDROID_NDK_ROOT=str(ndk),
                               PILON_BUILD_APP_ONLY="1", PILON_ENV_LOADED="1", PILON_ANDROID_BUILD_TYPE="Debug",
                               PATH=str(tools) + os.pathsep + environment["PATH"], PILON_PACKAGING_CALLS=str(calls))
            result = subprocess.run(["/bin/bash", str(ROOT / "src/scripts/android/build.sh")], cwd=temporary,
                                    env=environment, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            arguments = calls.read_bytes().split(b"\0")
            for expected in (ROOT / "src", ROOT / "build/android/app", ROOT / "addons/assistant/scripts/android-runtime-smoke.py",
                             ROOT / "build/android/app/android-build/build/outputs/apk/debug/android-build-debug.apk"):
                self.assertIn(os.fsencode(expected), arguments)
            self.assertFalse((temporary / "build").exists())

    def test_qt_source_extraction_resolves_patches_from_repository_root(self):
        script = (ROOT / "src/scripts/android/build-qt-platform.sh").read_text()
        programs = re.findall(r"<<'PY'\n(.*?)\nPY", script, re.S)
        program = next(code for code in programs if "'archive'" in code and "surface-lifecycle.patch" in code)
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            work = temporary / "build/android/qt-platform"
            work.mkdir(parents=True)
            archive = temporary / "source.tar"
            with tarfile.open(archive, "w"):
                pass
            tools = temporary / "bin"
            tools.mkdir()
            commands = {
                "git": '#!/bin/sh\ncat "$PILON_TEST_ARCHIVE"\n',
                "patch": '#!/bin/sh\nwhile [ "$#" -gt 0 ]; do\nif [ "$1" = -i ]; then shift; test -f "$1" || exit 9; printf "%s\\n" "$1" >> "$PILON_TEST_PATCHES"; fi\nshift\ndone\n',
            }
            for name, content in commands.items():
                command = tools / name
                command.write_text(content)
                command.chmod(0o755)
            environment = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"],
                               PILON_TEST_ARCHIVE=str(archive), PILON_TEST_PATCHES=str(temporary / "patches"))
            result = subprocess.run([sys.executable, "-", str(work), "fixture-commit", "fixture-input", str(ROOT)],
                                    input=program, env=environment, cwd=temporary,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((temporary / "patches").read_text().splitlines(), [
                str(ROOT / "src/platform/android/qt-platform/surface-lifecycle.patch"),
                str(ROOT / "src/platform/android/qt-platform/accessibility-activation.patch"),
                str(ROOT / "src/platform/android/qt-platform/ime-options.patch")])
            self.assertEqual((work / "source/.pilon-source-stamp").read_text(), "fixture-input")

    def test_headless_reminders_complete_qt_service_startup(self):
        main = (ROOT / "src/app/main.cpp").read_text()
        entry = main.split('QByteArray(argv[1]) == "--calendar-reminders"', 1)[1].split("return app.exec();", 1)[0]
        self.assertRegex(entry, r"\bQAndroidService\s+app\(argc, argv\);")
        self.assertNotRegex(entry, r"\bQ(?:Core|Gui)?Application\s+app\(")
        self.assertIn("#include <QtCore/private/qandroidextras_p.h>", main)
        cmake = (ROOT / "src/CMakeLists.txt").read_text()
        self.assertIn("list(APPEND PILON_QT_COMPONENTS CorePrivate)", cmake)
        self.assertRegex(cmake, r"target_link_libraries\(pilon PRIVATE Qt6::CorePrivate\)")
        manifest = ET.parse(ROOT / "src/platform/android/AndroidManifest.xml").getroot()
        android = "{http://schemas.android.com/apk/res/android}"
        service = next(item for item in manifest.find("application").findall("service")
                       if item.get(android + "name") == "org.pilon.PilonReminderService")
        self.assertEqual(service.get(android + "process"), ":calendar_reminders")
        self.assertEqual(service.get(android + "exported"), "false")
        arguments = next(item for item in service.findall("meta-data")
                         if item.get(android + "name") == "android.app.arguments")
        self.assertEqual(arguments.get(android + "value"), "--calendar-reminders")

    def test_backups_exclude_device_bound_credentials(self):
        required = {("file", ".pilon/assistant/codex"),
                    ("root", "app_org.pilon.Pilon"), ("file", "pilon-widgets/"),
                    ("sharedpref", "pilon_widgets.xml"), ("sharedpref", "pilon_widgets.xml.bak")}
        for name in ("pilon_credentials",
                     "com.microsoft.identity.client.account_credential_cache",
                     "com.microsoft.identity.client.single_account_credential_cache",
                     "com.microsoft.aad.adal.cache"):
            required.update(("sharedpref", name + suffix) for suffix in (".xml", ".xml.bak"))
        resources = ROOT / "src/platform/android/res/xml"
        policies = [ET.parse(resources / "pilon_backup_rules.xml").getroot()]
        policies.extend(ET.parse(resources / "pilon_data_extraction_rules.xml").getroot())
        self.assertEqual([policy.tag for policy in policies],
                         ["full-backup-content", "cloud-backup", "device-transfer"])
        for policy in policies:
            with self.subTest(policy=policy.tag):
                excluded = {(entry.get("domain"), entry.get("path"))
                            for entry in policy.findall("exclude")}
                self.assertFalse(required - excluded, required - excluded)

    def test_application_scan_retains_components_and_skips_nested_tests(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            names = ("Main.qml", "ui/Control.qml", "ui/Style.js", "tests/Test.qml", "tests/fixtures/Nested.qml")
            for name in names:
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text("import QtQuick\n")
            self.assertEqual(set(scanner.application_files(root)),
                             {str(root / name) for name in names[:3]})


if __name__ == "__main__":
    unittest.main()
