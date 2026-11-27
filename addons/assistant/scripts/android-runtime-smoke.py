#!/usr/bin/env python3
"""Check packaged ARM64 helpers and exercise them in an isolated USB-device folder."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import tomllib
import urllib.error
import urllib.request
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "src/scripts"))
from environment import development_root

DEV = development_root()
HELPERS = ("libpilon_codex.so", "libpilon_dns.so", "libpilon_llama.so")
MODEL_REVISION = "499bc8821c6b12b4e53c5bffcb21ec206f212d81"
MODEL_NAME = "stories15M-q4_0.gguf"
MODEL_SHA256 = "66967fbece6dbe97886593fdbb73589584927e29119ec31f08090732d1861739"


def elf_header(header):
    if len(header) < 64 or header[:6] != b"\x7fELF\x02\x01":
        raise ValueError("Runtime helper must be a little-endian ELF64 file.")
    kind, machine = struct.unpack_from("<HH", header, 16)
    if kind not in (2, 3) or machine != 183:
        raise ValueError("Runtime helper must be an ARM64 executable.")
    return kind


def check_alignment(source, name):
    header = source.read(64)
    elf_header(header)
    offset = struct.unpack_from("<Q", header, 32)[0]
    entry_size, count = struct.unpack_from("<HH", header, 54)
    if entry_size < 56 or not 0 < count <= 4096:
        raise ValueError(f"{name} has invalid ELF program headers.")
    loads = 0
    for index in range(count):
        source.seek(offset + index * entry_size)
        entry = source.read(56)
        if len(entry) != 56:
            raise ValueError(f"{name} has truncated ELF program headers.")
        if struct.unpack_from("<I", entry)[0] != 1:
            continue
        loads += 1
        alignment = struct.unpack_from("<Q", entry, 48)[0]
        if alignment < 16384 or alignment & (alignment - 1):
            raise ValueError(f"{name} requires rebuilding with 16 KB ELF alignment.")
    if not loads:
        raise ValueError(f"{name} has no ELF load segments.")


def check_apk(apk):
    count = 0
    with zipfile.ZipFile(apk) as archive:
        missing = [name for name in HELPERS if "lib/arm64-v8a/" + name not in archive.namelist()]
        if missing:
            raise ValueError("The APK is missing runtime helpers: " + ", ".join(missing))
        for entry in archive.infolist():
            if entry.filename.startswith("lib/") and entry.filename.endswith(".so"):
                with archive.open(entry) as source:
                    check_alignment(source, entry.filename)
                count += 1
    if not count:
        raise ValueError("The APK contains no native libraries.")
    print(f"APK native alignment verified: {count} ARM64 ELF files support 16 KB pages.")


def check_elf(runtime):
    system_libraries = {"libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libz.so"}
    for name in HELPERS:
        path = runtime / name
        with path.open("rb") as source:
            kind = elf_header(source.read(64))
            source.seek(0)
            check_alignment(source, name)
        program = subprocess.run(["readelf", "-l", str(path)], check=True,
                                 capture_output=True, text=True).stdout
        # Upstream Codex is fully static; Android's linker PIE requirement applies to dynamic helpers.
        if kind == 2 and "INTERP" in program:
            raise ValueError(f"{name} is a dynamically linked non-PIE executable.")
        dynamic = subprocess.run(["readelf", "-d", str(path)], check=True,
                                 capture_output=True, text=True).stdout
        needed = re.findall(r"\(NEEDED\).*\[(.*?)\]", dynamic)
        missing = [lib for lib in needed if lib not in system_libraries and not (runtime / lib).is_file()]
        if missing:
            raise ValueError(f"{name} is missing packaged libraries: {', '.join(missing)}")
        print(f"{name}: ARM64 {'PIE' if kind == 3 else 'static ELF'}; dependencies: {', '.join(needed) or 'static'}")


def verified_model(destination):
    def digest(path):
        with path.open("rb") as source:
            return hashlib.file_digest(source, "sha256").hexdigest()
    if destination.exists():
        if digest(destination) != MODEL_SHA256:
            raise ValueError(f"Model checksum mismatch: {destination}")
        return destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_suffix(".part")
    try:
        url = f"https://huggingface.co/ggml-org/models/resolve/{MODEL_REVISION}/tinyllamas/{MODEL_NAME}"
        with urllib.request.urlopen(url, timeout=60) as source, partial.open("wb") as output:
            shutil.copyfileobj(source, output)
        if digest(partial) != MODEL_SHA256:
            raise ValueError("Downloaded smoke model checksum mismatch.")
        partial.replace(destination)
    finally:
        partial.unlink(missing_ok=True)
    return destination


def completion_text(result):
    text = result.get("content")
    predicted = result.get("tokens_predicted", result.get("timings", {}).get("predicted_n", 0))
    if not isinstance(text, str) or not text.strip() or predicted <= 0:
        raise ValueError("The local model returned no generated tokens.")
    return text


def rpc_reply(process, expected_id, timeout=30):
    deadline = time.monotonic() + timeout
    buffer = b""
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        while time.monotonic() < deadline:
            if not selector.select(max(0, deadline - time.monotonic())):
                break
            data = os.read(process.stdout.fileno(), 65536)
            if not data:
                raise RuntimeError("Codex app-server closed before replying.")
            buffer += data
            if len(buffer) > 1024 * 1024:
                raise ValueError("Codex app-server returned an oversized response.")
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                message = json.loads(line)
                if message.get("id") != expected_id:
                    continue
                if "error" in message:
                    raise RuntimeError(f"Codex initialization failed: {message['error']}")
                if not isinstance(message.get("result"), dict):
                    raise ValueError("Codex app-server returned an invalid result.")
                return message["result"]
    raise TimeoutError("Codex app-server did not initialize within 30 seconds.")


def self_test():
    header = bytearray(64)
    header[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<HH", header, 16, 3, 183)
    elf_header(header)
    struct.pack_into("<H", header, 18, 62)
    try:
        elf_header(header)
        raise AssertionError("x86 runtime was accepted")
    except ValueError:
        pass
    struct.pack_into("<H", header, 18, 183)
    struct.pack_into("<Q", header, 32, 64)
    struct.pack_into("<HH", header, 54, 56, 1)
    program = bytearray(56)
    struct.pack_into("<I", program, 0, 1)
    with tempfile.TemporaryFile() as binary:
        for alignment in (4096, 16384, 65536):
            struct.pack_into("<Q", program, 48, alignment)
            binary.seek(0)
            binary.write(header + program)
            binary.seek(0)
            try:
                check_alignment(binary, "test helper")
                assert alignment >= 16384, "4 KB ELF was accepted"
            except ValueError:
                assert alignment < 16384, "16 KB-compatible ELF was rejected"
    assert completion_text({"content": " a story", "tokens_predicted": 2}) == " a story"
    for result in ({"content": "", "tokens_predicted": 2}, {"content": "x", "tokens_predicted": 0}):
        try:
            completion_text(result)
            raise AssertionError("empty generation was accepted")
        except ValueError:
            pass
    with tempfile.TemporaryFile() as error_log:
        child = subprocess.Popen(["python3", "-c", "import json; print(json.dumps({'id':7,'result':{'ok':True}}))"],
                                 stdout=subprocess.PIPE, stderr=error_log)
        assert rpc_reply(child, 7) == {"ok": True}
        assert child.wait(timeout=5) == 0
    print("Android runtime smoke self-checks passed.")


def device_smoke(args):
    release = ROOT / "addons/assistant/scripts/android/codex-release.toml"
    expected_version = tomllib.loads(release.read_text())["version"]
    runtime = args.runtime.resolve()
    check_elf(runtime)
    adb = [args.adb, "-d"]

    def command(*arguments, **kwargs):
        return subprocess.run(adb + list(arguments), check=True, capture_output=True,
                              text=True, timeout=30, **kwargs).stdout.strip()

    if command("get-state") != "device":
        raise RuntimeError("One authorized USB Android device is required.")
    if command("shell", "getprop ro.product.cpu.abi") != "arm64-v8a":
        raise RuntimeError("The USB device is not ARM64.")
    model = args.model or verified_model(DEV / "dev-deps/android/runtime" / "models" / MODEL_NAME)
    run_id = uuid.uuid4().hex
    remote = f"/data/local/tmp/pilon-runtime-probe/run-{run_id}"
    artifacts = DEV / "dev-deps/android/runtime" / "proof" / run_id
    artifacts.mkdir(parents=True)
    processes = []
    forwarded = None
    command("shell", f"mkdir -p {remote}/codex {remote}/tmp && chmod 700 {remote}")

    def launch(name, arguments, environment, log):
        pidfile = f"{remote}/{name}.pid"
        invocation = shlex.join(["env"] + [f"{key}={value}" for key, value in environment.items()]
                                + [f"{remote}/{name}"] + arguments)
        shell = f"cd {remote} && echo $$ > {pidfile} && exec {invocation}"
        child = subprocess.Popen(adb + ["shell", "-T", shell], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=log)
        processes.append((child, pidfile))
        return child

    try:
        for helper in runtime.glob("*.so"):
            command("push", str(helper), f"{remote}/{helper.name}")
        command("push", str(model.resolve()), f"{remote}/model.gguf")
        command("shell", f"chmod 700 {remote}/libpilon_codex.so {remote}/libpilon_llama.so")
        environment = {"HOME": remote, "CODEX_HOME": f"{remote}/codex", "TMPDIR": f"{remote}/tmp"}
        version = command("shell", shlex.join(["env"] + [f"{k}={v}" for k, v in environment.items()]
                                              + [f"{remote}/libpilon_codex.so", "--version"]))
        if not version.endswith(" " + expected_version):
            raise RuntimeError(f"Unexpected Codex version: {version}")
        with (artifacts / "codex.log").open("wb") as log:
            codex = launch("libpilon_codex.so", ["app-server", "--listen", "stdio://"], environment, log)
            request = {"id": 1, "method": "initialize", "params": {
                "clientInfo": {"name": "pilon", "title": "Pilon", "version": "1.0"},
                "capabilities": {"experimentalApi": True}}}
            codex.stdin.write(json.dumps(request).encode() + b"\n")
            codex.stdin.flush()
            initialized = rpc_reply(codex, 1)
            codex.stdin.write(b'{"method":"initialized"}\n')
            codex.stdin.flush()
            print("Codex version and app-server initialization passed on the USB device.")

        token = uuid.uuid4().hex
        remote_port = 20000 + int(run_id[:6], 16) % 30000
        with (artifacts / "llama.log").open("wb") as log:
            server = launch("libpilon_llama.so", ["--host", "127.0.0.1", "--port", str(remote_port),
                "--model", f"{remote}/model.gguf", "--ctx-size", "512", "--parallel", "1",
                "--gpu-layers", "0", "--threads", "2", "--no-webui"], {"LLAMA_API_KEY": token}, log)
            forwarded = command("forward", "tcp:0", f"tcp:{remote_port}")
            base_url = f"http://127.0.0.1:{forwarded}"
            headers = {"Authorization": f"Bearer {token}"}
            deadline = time.monotonic() + 120
            while True:
                if server.poll() is not None:
                    raise RuntimeError(f"llama-server stopped; see {artifacts / 'llama.log'}")
                try:
                    with urllib.request.urlopen(urllib.request.Request(base_url + "/health", headers=headers), timeout=2) as response:
                        if json.load(response).get("status") == "ok":
                            break
                except (OSError, urllib.error.HTTPError):
                    pass
                if time.monotonic() >= deadline:
                    raise TimeoutError("The phone's local model did not become ready.")
                time.sleep(0.25)
            body = json.dumps({"prompt": "Once upon a time", "n_predict": 16, "temperature": 0}).encode()
            request = urllib.request.Request(base_url + "/completion", data=body,
                                             headers={**headers, "Content-Type": "application/json"})
            with urllib.request.urlopen(request, timeout=60) as response:
                result = json.load(response)
            completion_text(result)
            proof = {"codex_version": version, "codex_initialize": bool(initialized),
                     "model": model.name, "completion": result, "scope": "adb shell runtime proof; APK proof is separate"}
            (artifacts / "result.json").write_text(json.dumps(proof, indent=2) + "\n")
            print(f"Offline ARM64 GGUF generation passed. Proof: {artifacts / 'result.json'}")
    finally:
        for child, pidfile in reversed(processes):
            try:
                pid = command("shell", f"cat {pidfile}")
                if pid.isdigit() and command("shell", f"readlink /proc/{pid}/exe") == pidfile[:-4]:
                    command("shell", f"kill -TERM {pid}")
            except subprocess.SubprocessError:
                pass
            if child.stdin:
                child.stdin.close()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=5)
        if forwarded:
            command("forward", "--remove", f"tcp:{forwarded}")
        command("shell", f"rm -rf {remote}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, default=DEV / "builds/android/runtime/arm64-v8a")
    parser.add_argument("--adb", default=shutil.which("adb") or str(Path.home() / "Android/Sdk/platform-tools/adb"))
    parser.add_argument("--model", type=Path, help="An existing GGUF; otherwise download the pinned public smoke model.")
    parser.add_argument("--check-elf", action="store_true")
    parser.add_argument("--check-apk", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    elif args.check_apk:
        check_apk(args.check_apk)
    elif args.check_elf:
        check_elf(args.runtime)
    else:
        device_smoke(args)


if __name__ == "__main__":
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "src/scripts"))
    from environment import load_environment
    load_environment()
    main()
