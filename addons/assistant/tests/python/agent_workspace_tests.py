import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/setup_agent_workspace.py"
spec = importlib.util.spec_from_file_location("setup_agent_workspace", SCRIPT)
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


class AgentWorkspaceTests(unittest.TestCase):
    def test_default_uses_pilons_selected_workspace(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "pilon"
            selected = root / "Selected Pilon Folder" / ".pilon/assistant/agent"
            executable.write_text(f"#!/bin/sh\ntest \"$1\" = --assistant-workspace && printf '%s\\n' '{selected}'\n")
            executable.chmod(0o700)
            with patch.dict(os.environ, {"PILON_AGENT_DIR": ""}):
                self.assertEqual(setup.workspace_path(executable), selected)
            with patch.dict(os.environ, {"PILON_AGENT_DIR": str(root / "override")}):
                self.assertEqual(setup.workspace_path(executable), root / "override")
            self.assertFalse(selected.exists())

    def test_only_manifests_and_instructions_are_seeded_from_legacy(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            destination = root / "selected/agent"
            legacy = root / "AgentFiles"
            (legacy / "tools/.venv").mkdir(parents=True)
            (legacy / "tools/.venv/keep").write_text("downloaded runtime")
            (legacy / "AGENTS.md").write_text("custom instructions")
            (legacy / "tools/pyproject.toml").write_text("custom manifest")
            (legacy / "tools/uv.lock").write_text("custom lock")
            with patch.object(setup.subprocess, "run"):
                setup.prepare(destination, legacy)
                (destination / "AGENTS.md").write_text("newer instructions")
                setup.prepare(destination, legacy)
            self.assertEqual((destination / "AGENTS.md").read_text(), "newer instructions")
            self.assertEqual((destination / "tools/pyproject.toml").read_text(), "custom manifest")
            self.assertEqual((destination / "tools/uv.lock").read_text(), "custom lock")
            self.assertFalse((destination / "tools/.venv").exists())
            self.assertEqual((legacy / "AGENTS.md").read_text(), "custom instructions")
            self.assertEqual((legacy / "tools/.venv/keep").read_text(), "downloaded runtime")


if __name__ == "__main__":
    unittest.main()
