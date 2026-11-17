#!/usr/bin/env python3
"""Exercise runnable artifacts in an owned folder, using the installed shared runtime."""
import argparse
import base64
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
import urllib.request
import uuid
import zipfile


def receive_exact(connection, size):
    data = b""
    while len(data) < size:
        part = connection.recv(size - len(data))
        if not part:
            raise AssertionError("Hot reload socket closed")
        data += part
    return data


def message(connection):
    header = receive_exact(connection, 2)
    size = header[1] & 127
    if size == 126:
        size = struct.unpack("!H", receive_exact(connection, 2))[0]
    elif size == 127:
        size = struct.unpack("!Q", receive_exact(connection, 8))[0]
    assert size < 1024 * 1024
    return json.loads(receive_exact(connection, size))


def diagnose_esbuild(root):
    directory = root / ("pilon-esbuild-proof-" + uuid.uuid4().hex)
    directory.mkdir(parents=True)
    try:
        source = directory / "input.js"
        source.write_text("export const proof = 42;\n")
        facts = {"scope":"Diagnostic only; not a successful project build", "python_entries":[{"name":entry.name,"inode":entry.inode()} for entry in os.scandir(directory)]}
        for version in ("123", "127"):
            binary = os.environ.get("PILON_GO_FS_PROBE_" + version)
            if binary:
                results = []
                for path in [directory,Path("/storage"),Path("/storage/emulated"),Path("/storage/emulated/0"),Path("/sdcard")]:
                    result = subprocess.run([binary,str(path)],cwd=directory,capture_output=True,text=True,timeout=15)
                    results.append({"path":str(path),"exit":result.returncode,"stdout":result.stdout,"stderr":result.stderr})
                facts["go_fs_" + version] = results
        binary = os.environ["ESBUILD_BINARY_PATH"]
        for label, args, content in [("absolute_file",[str(source)],None),("stdin",[],source.read_text())]:
            result = subprocess.run([binary,*args,"--bundle","--platform=node","--log-level=verbose"],cwd=directory,input=content,text=True,capture_output=True,timeout=15)
            facts[label] = {"exit":result.returncode,"stdout":result.stdout,"stderr":result.stderr}
        result = subprocess.run([os.environ["PILON_NODE"],"-e",'const fs=require("fs");const p=process.argv[1];console.log(JSON.stringify({entries:fs.readdirSync(p),stat:fs.statSync(p+"/input.js")}))',str(directory)],text=True,capture_output=True,timeout=10)
        facts["node"] = {"exit":result.returncode,"stdout":result.stdout,"stderr":result.stderr}
        if sys.platform == "android":
            import ctypes
            libc = ctypes.CDLL(None, use_errno=True)
            fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
            try:
                buffer = ctypes.create_string_buffer(8192)
                ctypes.set_errno(0)
                count = libc.syscall(ctypes.c_long(61),ctypes.c_int(fd),buffer,ctypes.c_size_t(len(buffer)))
                entries = [];offset = 0
                while offset < count:
                    inode,position,length,kind = struct.unpack_from("<QqHB",buffer.raw,offset)
                    assert length >= 20
                    name = buffer.raw[offset+19:offset+length].split(b"\0",1)[0].decode()
                    entries.append({"name":name,"inode":inode,"type":kind})
                    offset += length
                facts["raw_getdents64"] = {"count":count,"errno":ctypes.get_errno(),"entries":entries}
            finally:
                os.close(fd)
        return facts
    finally:
        shutil.rmtree(directory)


def run(creator, root, android_template=False):
    directory = root / ("pilon-runtime-proof-" + uuid.uuid4().hex)
    directory.mkdir(parents=True)
    facts = {}
    server = None
    package_name = "pilon_proof_" + uuid.uuid4().hex
    installed_package = Path(os.environ["PIP_TARGET"])
    try:
        spec = importlib.util.spec_from_file_location("creator", creator)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        platform = sys.platform
        if android_template:
            sys.platform = "android"
        try:
            python = module.create("python", "python-proof", directory, Path(os.environ["PILON_AGENT_TOOLS"]))
            react = module.create("react", "react-proof", directory, Path(os.environ["PILON_AGENT_TOOLS"]))
        finally:
            sys.platform = platform
        command = python.get("run_command", [sys.executable, python["run"]])
        package = directory / (package_name + "-1.0-py3-none-any.whl")
        with zipfile.ZipFile(package, "w") as wheel:
            wheel.writestr(package_name + ".py", 'value = "installed shared Python package"\n')
            info = package_name + "-1.0.dist-info/"
            wheel.writestr(info + "METADATA", "Metadata-Version: 2.1\nName: " + package_name + "\nVersion: 1.0\n")
            wheel.writestr(info + "WHEEL", "Wheel-Version: 1.0\nGenerator: Pilon fixture\nRoot-Is-Purelib: true\nTag: py3-none-any\n")
            wheel.writestr(info + "RECORD", "")
        subprocess.run([sys.executable, "-m", "pip", "install", "--no-index", "--no-deps", str(package)], check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        value = subprocess.check_output([sys.executable, "-c", "import " + package_name + ";print(" + package_name + ".value)"], text=True)
        assert value.strip() == "installed shared Python package"
        facts["shared_python_package"] = True
        project = Path(python["path"])
        (project / "main.py").write_text('import json, ssl, sqlite3, subprocess\nprint(json.dumps({"unicode":"π 日本語", "sqlite":sqlite3.sqlite_version, "ssl":ssl.OPENSSL_VERSION, "child":subprocess.check_output(["node","-e","process.stdout.write(String(7*6))"],text=True)}))\n')
        result = subprocess.check_output(command, text=True)
        facts["python"] = json.loads(result)
        assert facts["python"]["child"] == "42" and facts["python"]["unicode"] == "π 日本語"
        project = Path(react["path"])
        assert not (project / "node_modules").exists(), "Android artifacts must not need shared-storage symlinks"
        nested = project / "src/parts"
        nested.mkdir()
        source = nested / "message.js"
        source.write_text('export const message = "public-source-before";\n')
        (project / "src/main.jsx").write_text('import React from "react"; import {createRoot} from "react-dom/client"; import {message} from "./parts/message.js"; createRoot(document.getElementById("root")).render(<p>{message}</p>);\n')
        before = source.read_bytes()
        binary = os.environ["ESBUILD_BINARY_PATH"]
        for name in [project / "missing.js", source / "missing.js"]:
            result = subprocess.run([binary,str(name),"--bundle"],cwd=project,capture_output=True,text=True,timeout=15)
            assert result.returncode != 0 and "Could not resolve" in result.stderr
        facts["missing_and_non_directory_rejected"] = True
        started = time.monotonic()
        subprocess.run(react.get("run_command", [sys.executable, react["run"]]) + ["--build"], cwd=project, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        assert (project / "dist/index.html").is_file() and source.read_bytes() == before
        facts["react_build_passed"] = True
        facts["build_seconds"] = round(time.monotonic() - started, 3)
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        with (directory / "dev.log").open("wb") as log:
            server = subprocess.Popen(react.get("run_command", [sys.executable, react["run"]]) + ["--port", str(port), "--strictPort"], cwd=project, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            url = "http://127.0.0.1:" + str(port)
            deadline = time.monotonic() + 20
            while True:
                try:
                    with urllib.request.urlopen(url + "/@vite/client", timeout=1) as response:
                        client = response.read().decode()
                    break
                except OSError:
                    if server.poll() is not None or time.monotonic() > deadline:
                        raise AssertionError("Dev server did not become ready: " + (directory / "dev.log").read_text())
                    time.sleep(.05)
            with urllib.request.urlopen(url + "/src/main.jsx", timeout=5) as response:
                assert "/src/parts/message.js" in response.read().decode()
            with urllib.request.urlopen(url + "/src/parts/message.js", timeout=5) as response:
                assert "public-source-before" in response.read().decode()
            token = re.search(r'const wsToken = "([^"]+)"', client)
            assert token, "Vite client token missing"
            with socket.create_connection(("127.0.0.1", port), timeout=10) as connection:
                key = base64.b64encode(os.urandom(16)).decode()
                connection.sendall((f"GET /?token={token.group(1)} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: vite-hmr\r\n\r\n").encode())
                response = b""
                while not response.endswith(b"\r\n\r\n"):
                    response += receive_exact(connection, 1)
                assert response.startswith(b"HTTP/1.1 101")
                assert message(connection)["type"] == "connected"
                events = []
                for marker in ("public-source-edit-one", "public-source-after"):
                    time.sleep(.1)
                    source.write_text(before.decode().replace("public-source-before", marker))
                    update = message(connection)
                    assert update["type"] in ("update", "full-reload"), update
                    events.append(update["type"])
                facts["hot_reload_events"] = events
            with urllib.request.urlopen(url + "/src/parts/message.js", timeout=5) as response:
                assert "public-source-after" in response.read().decode()
            facts["publicWorkspaceVerified"] = sys.platform == "android" and str(root.resolve()).startswith("/storage/")
            facts["source_edit_preserved"] = "public-source-after" in source.read_text()
        return facts
    finally:
        if server is not None:
            import signal
            if server.poll() is None:
                os.killpg(server.pid, signal.SIGTERM)
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(server.pid, signal.SIGKILL)
                    server.wait(timeout=5)
        (installed_package / (package_name + ".py")).unlink(missing_ok=True)
        shutil.rmtree(installed_package / (package_name + "-1.0.dist-info"), ignore_errors=True)
        cache = installed_package / "__pycache__"
        if cache.exists():
            for file in cache.glob(package_name + ".*.pyc"):
                file.unlink()
        shutil.rmtree(directory)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--creator", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--android-template", action="store_true")
    parser.add_argument("--diagnose-esbuild", action="store_true")
    args = parser.parse_args()
    try:
        print(json.dumps(diagnose_esbuild(args.root) if args.diagnose_esbuild else run(args.creator, args.root, args.android_template), ensure_ascii=False))
    except subprocess.CalledProcessError as failure:
        if failure.output:
            print(failure.output.decode() if isinstance(failure.output, bytes) else failure.output, file=sys.stderr)
        raise
