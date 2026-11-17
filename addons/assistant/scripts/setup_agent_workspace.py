#!/usr/bin/env python3
"""Prepare the shared, persistent uv tool environment used by Pilon."""
import os
from pathlib import Path
import shutil
import subprocess


def workspace_path():
    if os.environ.get("PILON_AGENT_DIR"):
        return Path(os.environ["PILON_AGENT_DIR"])
    return Path(__file__).resolve().parents[3] / "AgentFiles"


def prepare(root):
    source = Path(__file__).resolve().parents[3] / "addons/assistant/tools"
    tools = root / "tools"
    tools.mkdir(parents=True, exist_ok=True)
    for name in ("pyproject.toml", "uv.lock"):
        if not (tools / name).exists():
            shutil.copyfile(source / name, tools / name)
    if not (root / "AGENTS.md").exists():
        shutil.copyfile(source / "AGENTS.md", root / "AGENTS.md")
    for name in ("browser_worker.py", "create_artifact.py"):
        shutil.copyfile(source / name, tools / name)
    env = {**os.environ, "UV_PROJECT_ENVIRONMENT": str(tools / ".venv"),
           "PLAYWRIGHT_BROWSERS_PATH": str(tools / "browsers")}
    subprocess.run(["uv", "sync", "--project", str(tools)], env=env, check=True)
    subprocess.run([str(tools / ".venv/bin/python"), "-m", "playwright", "install", "chromium"], env=env, check=True)
    return root


if __name__ == "__main__":
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "src/scripts"))
    from environment import load_environment
    load_environment()
    print(prepare(workspace_path()))
