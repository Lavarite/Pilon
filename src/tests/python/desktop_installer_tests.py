import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
import sys
from unittest import mock
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location("desktop_installer", ROOT / "src/scripts/install-desktop-entry.py")
installer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(installer)
sys.path.insert(0, str(ROOT / "src/scripts"))
PORTABLE_SPEC = importlib.util.spec_from_file_location("portable_package", ROOT / "src/scripts/package-release.py")
portable = importlib.util.module_from_spec(PORTABLE_SPEC)
PORTABLE_SPEC.loader.exec_module(portable)


class DesktopInstallerTests(unittest.TestCase):
    def test_portable_stages_the_installer_with_its_required_assets(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            portable.stage_source(ROOT, stage)
            spec = importlib.util.spec_from_file_location("staged_installer", stage / "src/scripts/install-desktop-entry.py")
            staged = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(staged)
            executable = stage / "pilon"
            executable.write_text("#!/bin/sh\nexit 0\n")
            executable.chmod(0o755)
            with mock.patch.dict(os.environ, {"XDG_DATA_HOME": str(stage / "data")}), mock.patch.object(staged.shutil, "which", return_value=None):
                self.assertTrue(staged.install(executable).is_file())

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.data = self.root / "user data"
        self.executable = self.root / 'build $`" path' / "pilon"
        self.executable.parent.mkdir()
        self.executable.write_text("#!/bin/sh\nexit 0\n")
        self.executable.chmod(0o755)
        self.environment = mock.patch.dict(os.environ, {"XDG_DATA_HOME": str(self.data)})
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def test_registers_types_and_distinct_icons_without_optional_commands(self):
        with mock.patch.object(installer.shutil, "which", return_value=None), mock.patch.object(installer.subprocess, "run") as run:
            desktop = installer.install(self.executable)
        run.assert_not_called()
        text = desktop.read_text()
        self.assertIn(' %F\n', text)
        self.assertIn("Icon=pilon\n", text)
        app_icon = self.data / "icons/hicolor/scalable/apps/pilon.svg"
        self.assertEqual(ET.parse(app_icon).getroot().attrib["viewBox"], "0 0 64 64")
        xml = self.data / "mime/packages/pilon.xml"
        definitions = ET.parse(xml).getroot()
        ns = {"mime": "http://www.freedesktop.org/standards/shared-mime-info"}
        entries = definitions.findall("mime:mime-type", ns)
        self.assertEqual({item.attrib["type"] for item in entries}, {
            "application/x-pilon-note", "application/x-pilon-calendar", "application/x-pilon-extension", "application/x-pilon-config"})
        self.assertEqual({glob.attrib["pattern"] for item in entries for glob in item.findall("mime:glob", ns)},
                         {"*.pin", "*.pical", "*.piext", "*.piconf", "*.piconfpack"})
        icons = []
        for entry in entries:
            if entry.attrib["type"] == "application/x-pilon-config":
                self.assertNotIn(entry.attrib["type"] + ";", text)
                self.assertEqual(entry.find("mime:sub-class-of", ns).attrib["type"], "text/plain")
                self.assertEqual({glob.attrib["pattern"] for glob in entry.findall("mime:glob", ns)},
                                 {"*.piconf", "*.piconfpack"})
            else:
                self.assertIn(entry.attrib["type"] + ";", text)
            name = entry.find("mime:icon", ns).attrib["name"]
            icon = self.data / "icons/hicolor/scalable/mimetypes" / (name + ".svg")
            self.assertEqual(ET.parse(icon).getroot().attrib["viewBox"], "0 0 256 256")
            icons.append(icon.read_bytes())
        self.assertEqual(len(set(icons)), 4)
        config_icon = self.data / "icons/hicolor/scalable/mimetypes/application-x-pilon-config.svg"
        self.assertEqual(config_icon.read_bytes(), (ROOT / "src/assets/icons/cog-6-tooth.svg").read_bytes())

    def test_runs_available_validators_and_database_updates(self):
        with mock.patch.object(installer.shutil, "which", side_effect=lambda name: "/mock/" + name), mock.patch.object(installer.subprocess, "run") as run:
            desktop = installer.install(self.executable)
        commands = [call.args[0] for call in run.call_args_list]
        self.assertEqual(commands[0][0], "/mock/desktop-file-validate")
        self.assertIn(["/mock/update-mime-database", str(self.data / "mime")], commands)
        self.assertIn(["/mock/gtk-update-icon-cache", "-f", "-t", str(self.data / "icons/hicolor")], commands)
        self.assertIn(["/mock/update-desktop-database", str(desktop.parent)], commands)
        self.assertTrue(all(call.kwargs["check"] for call in run.call_args_list))
        # Exec quotes are escaped for the desktop string before command unquoting.
        self.assertIn('\\\\$', desktop.read_text())
        self.assertIn('\\\\`', desktop.read_text())

    def test_refresh_keeps_the_selected_scheme_icon(self):
        icon = self.data / "icons/hicolor/scalable/apps/pilon.svg"
        icon.parent.mkdir(parents=True)
        selected = '<svg xmlns="http://www.w3.org/2000/svg" color="#b45f06"/>'
        icon.write_text(selected)
        with mock.patch.object(installer.shutil, "which", return_value=None):
            desktop = installer.install(self.executable)
        self.assertEqual(icon.read_text(), selected)
        self.assertIn("Icon=pilon\n", desktop.read_text())

    def test_rejects_unusable_executable_before_writing_registration(self):
        self.executable.chmod(0o644)
        with self.assertRaises(ValueError):
            installer.install(self.executable)
        self.assertFalse(self.data.exists())

    def test_failed_validation_preserves_existing_launcher(self):
        desktop = self.data / "applications/pilon.desktop"
        desktop.parent.mkdir(parents=True)
        desktop.write_text("existing launcher")
        with mock.patch.object(installer.shutil, "which", return_value="/mock/validator"), mock.patch.object(installer.subprocess, "run", side_effect=installer.subprocess.CalledProcessError(1, "validator")):
            with self.assertRaises(installer.subprocess.CalledProcessError):
                installer.install(self.executable)
        self.assertEqual(desktop.read_text(), "existing launcher")
        self.assertEqual(list(desktop.parent.glob("tmp*")), [])


class PublicInstallerTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.tools = self.root / "tools"
        self.tools.mkdir()
        self.assets = self.root / "assets"
        self.assets.mkdir()
        self.environment = {
            "PATH": str(self.tools), "HOME": str(self.root / "test home"),
            "PILON_INSTALL_ROOT": str(self.root / "Pilon install"),
            "PILON_TEST_ASSETS": str(self.assets), "PILON_TEST_ROOT": str(self.root),
        }
        Path(self.environment["HOME"]).mkdir()
        for command in ("awk", "cp", "grep", "mkdir", "mktemp", "mv", "rm", "sha256sum", "tar", "gzip"):
            (self.tools / command).symlink_to(shutil.which(command))
        (self.tools / "python3").symlink_to(sys.executable)
        self.command("id", "printf '1000\\n'")
        self.command("uname", "case $1 in -s) echo Linux;; -m) echo x86_64;; esac")
        self.command("ldd", "exit 0")
        self.command("getprop", """case $1 in
            ro.build.version.sdk) printf '%s\\n' "${PILON_TEST_SDK:-}";;
            ro.product.cpu.abilist) printf '%s\\n' "${PILON_TEST_ABIS:-}";;
            esac""")
        self.command("curl", """output= url=
            while [ "$#" -gt 0 ]; do
                case $1 in
                    --output) output=$2; shift;;
                    https://*) url=$1;;
                esac
                shift
            done
            printf '%s\\n' "$url" >> "$PILON_TEST_ROOT/downloads"
            case "$url" in
                https://api.github.com/repos/Lavarite/Pilon/releases*) name=releases.json;;
                https://github.com/Lavarite/Pilon/releases/download/v*/*) name=${url##*/};;
                *) exit 2;;
            esac
            cp "$PILON_TEST_ASSETS/$name" "$output"
            """)

    def command(self, name, body):
        executable = self.tools / name
        executable.write_text("#!/bin/sh\nset -eu\n" + body + "\n")
        executable.chmod(0o755)

    def published(self, version, *assets, draft=False):
        return {"tag_name": "v" + version, "draft": draft, "prerelease": True,
                "assets": [{"name": name, "state": "uploaded"} for name in assets]}

    def releases(self, *entries):
        (self.assets / "releases.json").write_text(json.dumps(entries))

    def checksum(self, asset, *, valid=True):
        stem = asset.removesuffix(".tar.gz").removesuffix(".apk")
        name = stem + "-SHA256SUMS"
        digest = hashlib.sha256((self.assets / asset).read_bytes()).hexdigest() if valid else "0" * 64
        (self.assets / name).write_text(f"{digest}  {asset}\n" + "1" * 64 + "  another-release-file\n")
        return name

    def linux_archive(self, layout):
        stem = "pilon-1.0.1-linux-x86_64"
        asset = stem + ".tar.gz"
        with tarfile.open(self.assets / asset, "w:gz") as archive:
            for path, content, mode in (
                (f"{layout}/pilon", b"#!/bin/sh\nexit 0\n", 0o755),
                (f"{layout}/addons/files.piext", b"Files package", 0o644),
                (f"{layout}/addons/.install-packages", b"", 0o644),
                ("src/scripts/install-desktop-entry.py", b"import os, sys\nfrom pathlib import Path\n"
                 b"Path(os.environ['PILON_TEST_ROOT'], 'launcher').write_text(sys.argv[-1])\n", 0o644),
            ):
                member = tarfile.TarInfo(f"{stem}/{path}")
                member.size = len(content)
                member.mode = mode
                archive.addfile(member, io.BytesIO(content))
        return asset

    def android_apk(self, *, valid=True):
        asset = "pilon-1.0.1-android-arm64-v8a.apk"
        (self.assets / asset).write_bytes(b"mock published APK")
        checksum = self.checksum(asset, valid=valid)
        self.releases(self.published("1.0.3", asset.replace("1.0.1", "1.0.3"),
                                     checksum.replace("1.0.1", "1.0.3"), draft=True),
                      self.published("1.0.2", asset.replace("1.0.1", "1.0.2")),
                      self.published("1.0.1", asset, checksum))
        return asset

    def run_installer(self, **environment):
        script = ROOT / "src/scripts/install.sh"
        return subprocess.run(["/bin/sh"], input=script.read_text(), text=True,
                              env={**self.environment, **environment}, capture_output=True, timeout=15)

    def test_linux_installs_current_and_legacy_release_layouts(self):
        for layout, destination in ((".bin", ".bin"), ("bin", "bin"), ("builds", "bin")):
            with self.subTest(layout=layout):
                asset = self.linux_archive(layout)
                self.releases(self.published("1.0.2", "pilon-1.0.2-android-arm64-v8a.apk"),
                              self.published("1.0.1", asset, self.checksum(asset)))
                prefix = self.root / f"Linux fixture {layout}"
                result = self.run_installer(PILON_INSTALL_ROOT=str(prefix))
                self.assertEqual(result.returncode, 0, result.stderr)
                binary = prefix / destination / "pilon"
                self.assertTrue(os.access(binary, os.X_OK))
                self.assertEqual((prefix / destination / "addons/files.piext").read_bytes(), b"Files package")
                self.assertEqual((self.root / "launcher").read_text(), str(binary))

    def test_linux_checksum_failure_preserves_existing_installation(self):
        asset = self.linux_archive(".bin")
        self.releases(self.published("1.0.1", asset, self.checksum(asset, valid=False)))
        binary = Path(self.environment["PILON_INSTALL_ROOT"], ".bin/pilon")
        binary.parent.mkdir(parents=True)
        binary.write_text("previous version")
        result = self.run_installer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SHA-256", result.stderr)
        self.assertEqual(binary.read_text(), "previous version")
        self.assertFalse((self.root / "launcher").exists())

    def test_android_hands_off_verified_file_and_keeps_it_after_exit(self):
        asset = self.android_apk()
        self.command("termux-open", "printf '%s\\n' \"$@\" > \"$PILON_TEST_ROOT/handoff\"")
        result = self.run_installer(PILON_TEST_SDK="28", PILON_TEST_ABIS="arm64-v8a,armeabi-v7a")
        self.assertEqual(result.returncode, 0, result.stderr)
        *arguments, path = (self.root / "handoff").read_text().splitlines()
        self.assertEqual(arguments, ["--view", "--content-type", "application/vnd.android.package-archive"])
        self.assertEqual(Path(path).name, asset)
        self.assertEqual(Path(path).read_bytes(), (self.assets / asset).read_bytes())
        self.assertIn(path, result.stdout)
        self.assertIn("Android", result.stdout)

    def test_android_without_handoff_keeps_the_file_and_prints_browser_link(self):
        asset = self.android_apk()
        for failed_opener in (False, True):
            with self.subTest(failed_opener=failed_opener):
                if failed_opener:
                    self.command("termux-open", "exit 1")
                result = self.run_installer(PILON_TEST_SDK="37", PILON_TEST_ABIS="arm64-v8a")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f"https://github.com/Lavarite/Pilon/releases/download/v1.0.1/{asset}", result.stdout)
                saved = list(Path(self.environment["HOME"]).rglob(asset))
                self.assertTrue(any(str(file) in result.stdout and file.is_file() for file in saved))

    def test_android_rejects_invalid_checksum_and_removes_partial_download(self):
        asset = self.android_apk(valid=False)
        self.command("termux-open", "printf 'opened\\n' > \"$PILON_TEST_ROOT/handoff\"")
        result = self.run_installer(PILON_TEST_SDK="37", PILON_TEST_ABIS="arm64-v8a")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SHA-256", result.stderr)
        self.assertFalse((self.root / "handoff").exists())
        self.assertFalse(list(Path(self.environment["HOME"]).rglob(asset)))

    def test_android_rejects_old_api_and_devices_without_arm64(self):
        for level, abis in (("27", "arm64-v8a"), ("28", "armeabi-v7a")):
            with self.subTest(level=level, abis=abis):
                result = self.run_installer(PILON_TEST_SDK=level, PILON_TEST_ABIS=abis)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Android", result.stderr)
                self.assertFalse((self.root / "downloads").exists())

    def test_android_recovers_to_browser_when_no_complete_release_exists(self):
        self.releases(self.published("1.0.1", "pilon-1.0.1-android-arm64-v8a.apk"))
        result = self.run_installer(PILON_TEST_SDK="28", PILON_TEST_ABIS="arm64-v8a")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("No published Android", result.stderr)
        self.assertIn("https://github.com/Lavarite/Pilon/releases", result.stderr)

    def test_android_without_python_prints_browser_fallback(self):
        (self.tools / "python3").unlink()
        result = self.run_installer(PILON_TEST_SDK="28", PILON_TEST_ABIS="arm64-v8a")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Missing required command: python3", result.stderr)
        self.assertIn("https://github.com/Lavarite/Pilon/releases", result.stderr)
        self.assertFalse((self.root / "downloads").exists())


if __name__ == "__main__":
    unittest.main()
