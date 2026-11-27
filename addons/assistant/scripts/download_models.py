#!/usr/bin/env python3
"""Download the Notes handwriting recognition models."""
import argparse
import concurrent.futures
import configparser
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "addons/assistant/config/assistant-models.json"
NOTES_MODEL = "ppocrv5"


def workspace():
    config = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
    pointer = configparser.ConfigParser(interpolation=None)
    pointer.read(config / "Pilon/workspace.ini", encoding="utf-8")
    return Path(pointer.get("workspace", "root", fallback=str(Path.home() / "Pilon")))


def download(item, root):
    override = os.environ.get("PILON_NOTES_MODELS_DIR")
    model_dir = Path(override).expanduser() if override else root / "addons" / "notes" / "data" / "models"
    target = model_dir / item["path"]
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_name(target.name + ".part")
    if not target.exists():
        print(f"Downloading {item['path']}", flush=True)
        subprocess.run(["curl", "--fail", "--location", "--silent", "--show-error",
                        "--retry", "5", "--retry-delay", "3", "--connect-timeout", "30",
                        "--continue-at", "-", "--output", str(partial), item["url"]], check=True)
    candidate = target if target.exists() else partial
    with candidate.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if candidate.stat().st_size != item["bytes"] or digest != item["sha256"]:
        raise RuntimeError(f"Integrity check failed: {candidate}; file retained for inspection")
    if candidate == partial:
        partial.replace(target)
    print(f"Verified {item['path']}", flush=True)


if __name__ == "__main__":
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "src/scripts"))
    from environment import load_environment
    load_environment()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, default=workspace())
    parser.add_argument("--model", default=NOTES_MODEL, choices=(NOTES_MODEL,))
    args = parser.parse_args()
    with MANIFEST.open() as stream:
        manifest = json.load(stream)
    if any(not item["path"].startswith(args.model + "/") for item in manifest["files"]):
        raise RuntimeError("The manifest must contain Notes handwriting recognition models only")
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        list(pool.map(lambda item: download(item, args.workspace.expanduser()),
                      manifest["files"]))
