import hashlib
import importlib.util
import io
import os
from pathlib import Path
import re
import shutil
import subprocess
import struct
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[4]
SPEC = importlib.util.spec_from_file_location("android_scan", ROOT / "src/scripts/android-qml-importscanner.py")
scanner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(scanner)


class AndroidPackagingTests(unittest.TestCase):
    def test_media_manifest_keeps_voice_optional_and_removes_qt_camera_permission(self):
        manifest = ET.parse(ROOT / "src/platform/android/AndroidManifest.xml").getroot()
        android = "{http://schemas.android.com/apk/res/android}"
        tools = "{http://schemas.android.com/tools}"
        permissions = {item.get(android + "name"): item for item in manifest.findall("uses-permission")}
        features = {item.get(android + "name"): item for item in manifest.findall("uses-feature")}
        self.assertEqual(permissions["android.permission.CAMERA"].get(tools + "node"), "remove")
        self.assertIn("android.permission.RECORD_AUDIO", permissions)
        self.assertEqual(features["android.hardware.microphone"].get(android + "required"), "false")
        self.assertEqual(features["android.hardware.bluetooth"].get(android + "required"), "false")

    def test_android_opens_the_retained_preview_with_read_only_access(self):
        files = (ROOT / "addons/files/src/remotefilemanager.cpp").read_text()
        preview = files.split("void RemoteFileManager::openPreviewExternally()", 1)[1].split("\n}\n", 1)[0]
        self.assertRegex(preview, r"#ifdef Q_OS_ANDROID\s+launchAttachment\(m_previewUrl, false, true\);")
        launch = files.split("void RemoteFileManager::launchAttachment(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn('openExternally ? "openFile" : "shareFile"', launch)
        self.assertIn('"org/pilon/PilonSharing", "retainFile"', launch)
        self.assertIn("openExternally ? QUrl{} : url", launch)
        sharing = (ROOT / "src/platform/android/src/org/pilon/PilonSharing.java").read_text()
        self.assertIn("Intent.ACTION_VIEW).setDataAndType(uri, mimeType)", sharing)
        self.assertIn("FileProvider.getUriForFile", sharing)
        self.assertIn("FLAG_GRANT_READ_URI_PERMISSION", sharing)
        self.assertNotIn("FLAG_GRANT_WRITE_URI_PERMISSION", sharing)

    def test_files_audio_preview_uses_the_packaged_ffmpeg(self):
        files = (ROOT / "addons/files/src/remotefilemanager.cpp").read_text()
        resolver = files.split("QString previewFfmpeg()", 1)[1].split("\n}\n", 1)[0]
        self.assertRegex(resolver, r'#ifdef Q_OS_ANDROID\s+return '
                                   r'PilonWorkspace::packagedExecutable\(QStringLiteral\("ffmpeg"\)\);')
        preview = files.split("QVariantMap previewFile(", 1)[1].split("QVariantMap executeRequest(", 1)[0]
        self.assertIn("const auto ffmpeg = previewFfmpeg();", preview)
        self.assertIn("decoder.start(ffmpeg,", preview)
        cmake = (ROOT / "src/CMakeLists.txt").read_text()
        self.assertRegex(cmake, r"foreach\(runtime IN ITEMS [^)]+\bffmpeg\b[^)]*\)")

    def test_worker_only_libraries_are_packaged_without_qt_startup_loading(self):
        cmake = (ROOT / "src/CMakeLists.txt").read_text()
        preloads = re.search(r'QT_ANDROID_EXTRA_LIBS\s+"([^"]*)"', cmake)
        self.assertIsNotNone(preloads)
        worker_libraries = re.search(r"foreach\(library IN ITEMS ([^)]+)\)", cmake)
        self.assertIsNotNone(worker_libraries)
        for library in ("onnxruntime", "audiocpp"):
            self.assertNotIn(f"lib{library}.so", preloads.group(1))
            self.assertIn(library, worker_libraries.group(1).split())

    def test_libcore_checks_source_bytes_across_regenerated_archives(self):
        import android_widget_tests

        content = b"package org.json; class Sample {}\n"
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(
                android_widget_tests, "JSON_SOURCES", {"Sample.java": hashlib.sha256(content).hexdigest()}):
            root = Path(directory)
            archive_path = root / "source.tar.gz"

            def pack(mtime, java):
                with tarfile.open(archive_path, "w:gz") as archive:
                    source = tarfile.TarInfo("Sample.java")
                    source.mtime, source.size = mtime, len(java)
                    archive.addfile(source, io.BytesIO(java))

            for timestamp in (1, 2):
                pack(timestamp, content)
                source, = android_widget_tests.libcore_json_sources(root, archive_path)
                self.assertEqual(source.read_bytes(), content)
            pack(3, content + b"// unexpected\n")
            with self.assertRaisesRegex(ValueError, "checksum does not match: Sample.java"):
                android_widget_tests.libcore_json_sources(root, archive_path)

    def test_parked_qt_surface_retires_exposure_before_late_gui_cleanup(self):
        patch = (ROOT / "src/platform/android/qt-platform/surface-lifecycle.patch").read_text()
        applied = "\n".join(line[1:] for line in patch.splitlines() if line.startswith(("+", " ")))
        release = applied.split("void QAndroidPlatformWindow::releaseSurface()", 1)[1].split("\n}\n", 1)[0]
        self.assertRegex(release, r"m_surfaceValid = false;\s*destroyWindowSurface\(\);")
        self.assertIn("std::atomic_bool m_surfaceValid", applied)
        native = applied.split("void QAndroidPlatformWindow::surfaceDestroyed(JNIEnv *env", 1)[1].split("\n}\n", 1)[0]
        self.assertRegex(native, r"if \(parked\) \{\s*if \(auto \*pw = findWindowById\(windowId\)\)\s*"
                                 r"pw->releaseSurface\(\);\s*finish\(\);\s*\}\s*QMetaObject::invokeMethod")
        self.assertRegex(native, r"pw->onSurfaceDestroyed\(\);\s*if \(!parked\)\s*finish\(\);")

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

    def test_qt_input_event_bytecode_uses_both_ime_masks(self):
        spec = importlib.util.spec_from_file_location("qt_proof", Path(__file__).with_name("android_qt_platform_smoke.py"))
        proof = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(proof)
        original = ["Class descriptor  : 'Lorg/qtproject/qt/android/QtInputConnection;'",
                    "name          : 'sendKeyEvent'", "|0011: iget v0, v0, QtEditText;.m_imeOptions:I",
                    "|0013: sparse-switch v0, 00000070"]
        masks = ["|0013: const/high16 v1, #int 1073741824",
                 "|0015: and-int v2, v0, v1", "|001a: and-int/lit16 v1, v0, #int 255"]
        self.assertFalse(proof.patched_ime_dispatch(original))
        self.assertFalse(proof.patched_ime_dispatch([*original, "name : 'unrelated'", *masks]))
        self.assertFalse(proof.patched_ime_dispatch([*original[:-1], *masks[:-1], original[-1]]))
        self.assertTrue(proof.patched_ime_dispatch([*original[:-1], *masks, original[-1]]))
        with self.assertRaisesRegex(ValueError, "Unexpected Android bytecode"):
            list(proof.dex_classes(b"invalid bytecode"))

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
            go = tools / "go"
            go.write_text('#!/bin/sh\ntest "$GOTOOLCHAIN" = local && printf "go1.27.3\\n"\n')
            go.chmod(0o755)
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
            for name in ("PILON_ANDROID_BUILD_DIR", "PILON_DEV_DIR", "XDG_CACHE_HOME"):
                environment.pop(name, None)
            environment.update(HOME=str(home), JAVA_HOME=str(jdk), PILON_ANDROID_QT=str(android_qt),
                               QT_HOST_PATH=str(host_qt), ANDROID_SDK_ROOT=str(sdk), ANDROID_NDK_ROOT=str(ndk),
                               PILON_BUILD_APP_ONLY="1", PILON_ENV_LOADED="1", PILON_ANDROID_BUILD_TYPE="Debug",
                               PATH=str(tools) + os.pathsep + environment["PATH"], PILON_PACKAGING_CALLS=str(calls))
            for overrides, development in (({}, home / ".cache/pilon-dev"),
                    ({"XDG_CACHE_HOME": str(temporary / "cache")}, temporary / "cache/pilon-dev"),
                    ({"PILON_DEV_DIR": str(temporary / "explicit development")}, temporary / "explicit development"),
                    ({"PILON_ANDROID_BUILD_DIR": str(temporary / "alternate-app")}, home / ".cache/pilon-dev")):
                with self.subTest(overrides=overrides):
                    calls.write_bytes(b"")
                    app_build = Path(overrides.get("PILON_ANDROID_BUILD_DIR", development / "builds/work/android/app"))
                    old_apk = app_build / "android-build/pilon.apk"
                    old_apk.parent.mkdir(parents=True, exist_ok=True)
                    old_apk.touch()
                    result = subprocess.run(["/bin/bash", str(ROOT / "src/scripts/android/build.sh")], cwd=temporary,
                                            env=environment | overrides, capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertFalse(old_apk.exists())
                    arguments = calls.read_bytes().split(b"\0")
                    expected = (ROOT / "src", app_build, f"-DPILON_DEV_DIR={development}",
                                ROOT / "addons/assistant/scripts/android-runtime-smoke.py",
                                app_build / "android-build/build/outputs/apk/debug/android-build-debug.apk")
                    for value in expected:
                        self.assertIn(os.fsencode(value), arguments)
            self.assertFalse((temporary / "builds").exists())

    def test_qt_source_extraction_resolves_patches_from_repository_root(self):
        script = (ROOT / "src/scripts/android/build-qt-platform.sh").read_text()
        programs = re.findall(r"<<'PY'\n(.*?)\nPY", script, re.S)
        program = next(code for code in programs if "'archive'" in code and "surface-lifecycle.patch" in code)
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            work = temporary / "dev-deps/android/qt-platform"
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
        expected = {(entry.get("domain"), entry.get("path")) for entry in policies[0].findall("exclude")}
        for policy in policies:
            with self.subTest(policy=policy.tag):
                excluded = {(entry.get("domain"), entry.get("path"))
                            for entry in policy.findall("exclude")}
                self.assertFalse(required - excluded, required - excluded)
                self.assertEqual(excluded, expected)

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
