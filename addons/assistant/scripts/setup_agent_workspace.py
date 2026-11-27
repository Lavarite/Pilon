#!/usr/bin/env python3
"""Prepare the shared, persistent uv tool environment used by Pilon."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def find_pilon(executable=None):
    if executable is not None:
        return executable.resolve(strict=True)
    installed_root = Path(os.environ.get("PILON_INSTALL_ROOT") or Path.home() / "Pilon")
    for local in (ROOT / ".bin/pilon", installed_root / ".bin/pilon",
                  ROOT / "bin/pilon", installed_root / "bin/pilon", ROOT / "builds/pilon"):
        if os.access(local, os.X_OK):
            return local.resolve()
    installed = shutil.which("pilon")
    if installed:
        return Path(installed).resolve()
    raise FileNotFoundError("Build Pilon first, pass --pilon, or set PILON_AGENT_DIR.")


def workspace_path(executable=None):
    if os.environ.get("PILON_AGENT_DIR"):
        return Path(os.environ["PILON_AGENT_DIR"])
    result = subprocess.run([str(find_pilon(executable)), "--assistant-workspace"],
                            capture_output=True, check=True, timeout=5)
    root = Path(os.fsdecode(result.stdout.rstrip(b"\n")))
    if not root.is_absolute() or root.parts[-3:] != (".pilon", "assistant", "agent"):
        raise ValueError("Pilon did not return an Assistant workspace path.")
    return root


def seed(source, destination, replace=False):
    if not replace and (destination.exists() or destination.is_symlink()):
        return
    if not replace:
        content = source.read_bytes()
        try:
            with destination.open("xb") as output:
                output.write(content)
        except FileExistsError:
            pass
        return
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as staged:
        path = Path(staged.name)
        try:
            with source.open("rb") as original:
                shutil.copyfileobj(original, staged)
            staged.close()
            path.replace(destination)
        finally:
            path.unlink(missing_ok=True)


def prepare(root, legacy=None):
    source = ROOT / "addons/assistant/tools"
    tools = root / "tools"
    tools.mkdir(parents=True, exist_ok=True)
    for name in ("AGENTS.md", "pyproject.toml", "uv.lock"):
        relative = Path(name) if name == "AGENTS.md" else Path("tools") / name
        former = legacy / relative if legacy and not legacy.is_symlink() else None
        if former and (former.is_symlink() or former.parent.is_symlink() or not former.is_file()):
            former = None
        seed(former or source / name, root / relative)
    for name in ("browser_worker.py", "create_artifact.py"):
        seed(source / name, tools / name, replace=True)
    env = {**os.environ, "UV_PROJECT_ENVIRONMENT": str(tools / ".venv"),
           "PLAYWRIGHT_BROWSERS_PATH": str(tools / "browsers")}
    subprocess.run(["uv", "sync", "--project", str(tools)], env=env, check=True)
    subprocess.run([str(tools / ".venv/bin/python"), "-m", "playwright", "install", "chromium"], env=env, check=True)
    return root


if __name__ == "__main__":
    import sys
    sys.path.insert(0, str(ROOT / "src/scripts"))
    from environment import load_environment
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pilon", type=Path, help="Pilon executable that reads the selected workspace")
    arguments = parser.parse_args()
    try:
        load_environment()
        executable = None if os.environ.get("PILON_AGENT_DIR") else find_pilon(arguments.pilon)
        root = workspace_path(executable)
        legacy = executable.parent.parent / "AgentFiles" if executable else None
        if executable:
            root.parents[1].mkdir(mode=0o700, parents=True, exist_ok=True)
        print(prepare(root, legacy))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
