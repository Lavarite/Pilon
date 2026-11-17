"""Optional real-worker regression: python3 <this file> MODEL.gguf [--mmproj FILE]."""

import argparse
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path)
    parser.add_argument("--mmproj", type=Path)
    parser.add_argument("--runtime", type=Path, default=Path(__file__).resolve().parents[4] / "runtime/bin/llama-server")
    parser.add_argument("--cpu", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pilon-cache-regression-") as folder:
        process = None
        port = 0

        def request(path, data=None):
            req = urllib.request.Request(f"http://127.0.0.1:{port}{path}",
                data=None if data is None else json.dumps(data).encode(),
                headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=300) as response:
                return json.load(response)

        def stop():
            nonlocal process
            if process is not None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                process = None

        def start(log):
            nonlocal process, port
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                port = reservation.getsockname()[1]
            command = [str(args.runtime.resolve()), "--model", str(args.model.resolve()),
                "--host", "127.0.0.1", "--port", str(port), "--ctx-size", "4096",
                "--parallel", "1", "--batch-size", "128", "--ubatch-size", "128",
                "--slot-save-path", folder, "--no-webui", "--no-warmup"]
            if args.mmproj:
                command += ["--mmproj", str(args.mmproj.resolve())]
            if args.cpu:
                command += ["--device", "none", "--gpu-layers", "0", "--no-kv-offload", "--no-mmproj-offload"]
            process = subprocess.Popen(command, stdout=log, stderr=log)
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline and process.poll() is None:
                try:
                    request("/health")
                    return
                except (urllib.error.URLError, TimeoutError):
                    time.sleep(0.1)
            raise RuntimeError("Worker failed to start")

        with tempfile.TemporaryFile(mode="w+") as log:
            try:
                start(log)
                prefix = request("/tokenize", {"content": "These are the fixed instructions. " * 100,
                    "add_special": True, "parse_special": True})["tokens"]
                assert len(prefix) > 512, "Fixture must exceed Gemma's sliding window"
                request("/completion", {"prompt": prefix, "n_predict": 0, "cache_prompt": True, "id_slot": 0})
                saved = request("/slots/0?action=save", {"filename": "instructions.bin"})
                assert saved["n_saved"] == len(prefix), saved
                stop()
                start(log)
                restored = request("/slots/0?action=restore", {"filename": "instructions.bin"})
                assert restored["n_restored"] == len(prefix), restored
                suffix = request("/tokenize", {"content": " Reply briefly.", "add_special": False})["tokens"]
                result = request("/completion", {"prompt": prefix + suffix, "n_predict": 1, "cache_prompt": True})
                assert result["stop"], result
                assert result["timings"]["cache_n"] == len(prefix), result["timings"]
                changed = request("/tokenize", {"content": "Different instructions. Reply briefly.", "add_special": True})["tokens"]
                result = request("/completion", {"prompt": changed, "n_predict": 1, "cache_prompt": True})
                assert result["stop"], result
                assert result["timings"]["cache_n"] < len(changed), result["timings"]
                print(f"PASS: restored {len(prefix)} instruction tokens after worker restart; changed prefix reevaluated")
            except Exception:
                log.seek(0)
                print(log.read()[-16000:])
                raise
            finally:
                stop()


if __name__ == "__main__":
    main()
