import importlib.util
import os
from pathlib import Path
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
            "application/x-pilon-note", "application/x-pilon-calendar", "application/x-pilon-extension"})
        self.assertEqual({glob.attrib["pattern"] for item in entries for glob in item.findall("mime:glob", ns)}, {"*.pin", "*.pical", "*.piext"})
        icons = []
        for entry in entries:
            self.assertIn(entry.attrib["type"] + ";", text)
            name = entry.find("mime:icon", ns).attrib["name"]
            icon = self.data / "icons/hicolor/scalable/mimetypes" / (name + ".svg")
            self.assertEqual(ET.parse(icon).getroot().attrib["viewBox"], "0 0 256 256")
            icons.append(icon.read_bytes())
        self.assertEqual(len(set(icons)), 3)

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


if __name__ == "__main__":
    unittest.main()
