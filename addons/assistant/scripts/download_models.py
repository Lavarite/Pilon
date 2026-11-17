#!/usr/bin/env python3
"""Optional command-line download of recognition and speech models."""
import concurrent.futures
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "addons/assistant/config/assistant-models.json"


def download(item):
    target = ROOT / "models" / item["path"]
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_name(target.name + ".part")
    if not target.exists():
        url = item.get("url") or "https://huggingface.co/{}/resolve/{}/{}".format(
            item["repository"], item["revision"], item["filename"])
        print(f"Downloading {item['path']}", flush=True)
        subprocess.run(["curl", "--fail", "--location", "--silent", "--show-error",
                        "--retry", "5", "--retry-delay", "3", "--connect-timeout", "30",
                        "--continue-at", "-", "--output", str(partial), url], check=True)
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
    with MANIFEST.open() as stream:
        manifest = json.load(stream)
    if any(not item["path"].startswith(("whisper-large-v3-turbo/", "breeze-tts-2/", "ppocrv5/"))
           for item in manifest["files"]):
        raise RuntimeError("The manifest must contain recognition and speech support models only")
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        list(pool.map(download, (item for item in manifest["files"] if item.get("platform") != "android")))
