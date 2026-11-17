import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/build_assistant_runtime.sh"


class RuntimeBuildTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / "src/scripts").mkdir(parents=True)
        (self.root / "addons/assistant/scripts").mkdir(parents=True)
        shutil.copyfile(SCRIPT, self.root / "addons/assistant/scripts/build_assistant_runtime.sh")
        shutil.copyfile(Path(__file__).resolve().parents[4] / "src/scripts/environment.py", self.root / "src/scripts/environment.py")
        for path in ("addons/assistant/config/llama-prefix-cache.patch", "runtime/llama-prefix-cache.patch"):
            destination = self.root / path
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text("matching fixture patch")
        for name in ("llama-server", "whisper-cli"):
            path = self.root / "runtime/bin" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture runtime")
            path.chmod(0o600)
        tools = self.root / "tools"
        tools.mkdir()
        for name in ("dirname", "cat", "cmp", "chmod", "python3", "bash"):
            (tools / name).symlink_to(shutil.which(name))
        self.env = {**os.environ, "PATH": str(tools), "PILON_REBUILD_RUNTIME": "0", "PILON_RELEASE_ENV": "", "PILON_ENV_LOADED": "0"}

    def run_script(self, backend):
        return subprocess.run(["/bin/bash", str(self.root / "addons/assistant/scripts/build_assistant_runtime.sh")],
                              env={**self.env, "PILON_LLM_BACKEND": backend}, capture_output=True, text=True)

    def test_auto_repairs_transferred_modes_without_compiler_or_download(self):
        result = self.run_script("auto")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(os.access(self.root / "runtime/bin/llama-server", os.X_OK))
        self.assertTrue(os.access(self.root / "runtime/bin/whisper-cli", os.X_OK))

    def test_existing_cuda_runtime_needs_no_compiler(self):
        path = self.root / "runtime/cuda/bin/llama-server"
        path.parent.mkdir(parents=True)
        path.write_text("fixture cuda")
        shutil.copyfile(self.root / "addons/assistant/config/llama-prefix-cache.patch", self.root / "runtime/cuda/llama-prefix-cache.patch")
        result = self.run_script("cuda")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(os.access(path, os.X_OK))

    def test_explicit_cuda_build_reports_missing_compiler(self):
        result = self.run_script("cuda")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("nvcc", result.stderr)
        self.assertIn("PILON_LLM_BACKEND=vulkan", result.stderr)


if __name__ == "__main__":
    unittest.main()
