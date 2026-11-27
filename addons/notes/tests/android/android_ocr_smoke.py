#!/usr/bin/env python3
"""Exercise the Android OCR worker on an authorized USB phone, outside user data."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import shlex
import subprocess
import sys
import time
import uuid


ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "src/scripts"))
from environment import development_root

DEV = development_root()
FIXTURES = ROOT / "addons/notes/tests/fixtures/android-ocr"
EXPECTED = ("Hello", "Imported meeting agenda")


def checked_lines(tiles):
    assert len(tiles) == len(EXPECTED), tiles
    for detections, expected in zip(tiles, EXPECTED):
        assert len(detections) == 1, detections
        line = detections[0]
        assert line["text"] == expected, line
        assert line["score"] > .9, line
        encoded = expected.encode("utf-16-le")
        recovered = []
        previous = -1
        for word in line["words"]:
            start, length = word["start"], word["length"]
            assert isinstance(start, int) and isinstance(length, int)
            assert start > previous and length > 0 and (start + length) * 2 <= len(encoded)
            recovered.append(encoded[start * 2:(start + length) * 2].decode("utf-16-le"))
            previous = start
            assert all(math.isfinite(word[key]) for key in ("x", "y", "width", "height"))
            assert word["width"] > 0 and word["height"] > 0
            assert line["x"] - 2 <= word["x"] <= line["x"] + line["width"] + 2
            assert line["y"] - 2 <= word["y"] <= line["y"] + line["height"] + 2
        assert "".join(recovered) == expected.replace(" ", ""), line


def check_alignment(path):
    output = subprocess.check_output(["readelf", "-lW", str(path)], text=True)
    loads = [row.split() for row in output.splitlines() if row.strip().startswith("LOAD")]
    assert loads and all(int(row[-1], 16) >= 16384 for row in loads), (path, loads)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", type=Path, default=Path.home() / "Android/Sdk/platform-tools/adb")
    parser.add_argument("--models", type=Path, default=ROOT / "models/ppocrv5")
    parser.add_argument("--qt", type=Path, default=Path.home() / "Qt/6.12.0/android_arm64_v8a")
    parser.add_argument("--ndk", type=Path, default=Path.home() / "Android/Sdk/ndk/27.2.12479018")
    parser.add_argument("--output", type=Path, default=DEV / "dev-deps/android/ocr/proof/smoke.json")
    args = parser.parse_args()
    runtime = DEV / "builds/android/runtime/arm64-v8a"
    libraries = [runtime / "libpilon_ocr.so", runtime / "libonnxruntime.so",
                 args.qt / "lib/libQt6Core_arm64-v8a.so",
                 args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"]
    for library in libraries:
        check_alignment(library)
    manifest = json.loads((ROOT / "addons/assistant/config/assistant-models.json").read_text())
    models = []
    for name in ("ch_PP-OCRv5_det_server.onnx", "en_PP-OCRv5_rec_mobile.onnx"):
        path = args.models / name
        expected = next(file["sha256"] for file in manifest["files"] if file["path"] == "ppocrv5/" + name)
        with path.open("rb") as source:
            assert hashlib.file_digest(source, "sha256").hexdigest() == expected, path
        models.append(path)

    def command(*arguments, **kwargs):
        return subprocess.run([str(args.adb), "-d", *arguments], check=True,
                              capture_output=True, **kwargs)

    remote = "/data/local/tmp/pilon-ocr-probe-" + uuid.uuid4().hex
    proof = {"scope": "native worker under isolated shell UID; app integration is a separate check"}
    try:
        command("shell", "mkdir", "-p", remote + "/models")
        for path in libraries:
            command("push", str(path), remote + "/" + path.name)
        for path in models:
            command("push", str(path), remote + "/models/" + path.name)
        for name in ("ink.png", "printed.png"):
            command("push", str(FIXTURES / name), remote + "/" + name)
        command("shell", "chmod", "700", remote + "/libpilon_ocr.so")
        program = "env LD_LIBRARY_PATH=" + shlex.quote(remote) + " " + shlex.quote(remote + "/libpilon_ocr.so")
        proof["self_test"] = command("shell", program + " --self-test", text=True).stdout.strip()
        tiles = [{"path": remote + "/" + name, "scale": 1., "x": 0., "y": 0.,
                  "coreX": 0., "coreY": 0., "coreSize": 1024.} for name in ("ink.png", "printed.png")]
        shifted = [dict(tile, scale=1.5, x=300., y=400., coreX=300., coreY=400.) for tile in tiles]
        excluded = [dict(tile, coreX=1000., coreY=1000.) for tile in tiles]
        requests = [{"lines": tiles}, {"lines": shifted}, {"lines": excluded},
                    {"lines": [dict(tiles[0], scale=0)]}, {"lines": "invalid"}, {"lines": tiles}]
        started = time.monotonic()
        result = command("shell", program + " " + shlex.quote(remote + "/models"),
                         input="".join(json.dumps(request) + "\n" for request in requests), text=True, timeout=180)
        proof.update(seconds=time.monotonic() - started, stderr=result.stderr)
        replies = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
        replies = [reply for reply in replies if "progress" not in reply]
        proof["replies"] = replies
        assert len(replies) == len(requests), replies
        checked_lines(replies[0]["tiles"])
        checked_lines(replies[1]["tiles"])
        for baseline, moved in zip(replies[0]["tiles"], replies[1]["tiles"]):
            for old, new in zip([baseline[0], *baseline[0]["words"]], [moved[0], *moved[0]["words"]]):
                for key, offset in (("x", 300), ("y", 400), ("width", 0), ("height", 0)):
                    assert abs(new[key] - (old[key] / 1.5 + offset)) < .01, (key, old, new)
        assert replies[2]["tiles"] == [[], []], replies[2]
        assert "error" in replies[3] and "error" in replies[4], replies
        checked_lines(replies[5]["tiles"])
        proof["passed"] = ["handwritten raster", "printed image", "CTC UTF-16 positions",
                           "scaled document bounds", "tile ownership", "invalid request recovery", "16KB ELF alignment"]
    finally:
        cleanup = subprocess.run([str(args.adb), "-d", "shell", "rm", "-rf", remote], capture_output=True)
        proof["cleaned"] = cleanup.returncode == 0
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(proof, indent=2) + "\n")
        if cleanup.returncode:
            raise RuntimeError("The isolated device test folder could not be removed: " + remote)
    print("Android OCR checks passed:", ", ".join(proof["passed"]))
    print("Evidence:", args.output)


if __name__ == "__main__":
    main()
