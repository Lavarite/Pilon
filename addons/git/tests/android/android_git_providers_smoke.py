#!/usr/bin/env python3
"""Verify native provider clients and fixture-only credential transport without UI or accounts."""
import json
from pathlib import Path, PurePosixPath
import re
import select
import socket as host_socket_api
import shlex
import subprocess
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[4]
ADB = [str(Path.home() / "Android/Sdk/platform-tools/adb"), "-d"]
PACKAGE = "org.pilon.PilonProviderProbe"


def run(arguments, check=True):
    result = subprocess.run(arguments, capture_output=True, text=True, timeout=30)
    if check and result.returncode:
        raise RuntimeError(result.stderr[-2000:] + result.stdout[-2000:])
    return result


def main():
    work = ROOT / "build/android/git-providers"
    proof = Path(tempfile.mkdtemp(prefix="device-proof-", dir=work))
    remote = None
    installed = False
    sockets = tempfile.TemporaryDirectory(prefix="pilon-provider-")
    server = None
    reverse = None
    try:
        if run(ADB + ["shell", "pm", "path", PACKAGE], check=False).stdout.strip():
            raise RuntimeError("Refusing to replace an existing provider probe package.")
        run(ADB + ["install", "-t", str(work / "probe-apk/provider-probe.apk")])
        installed = True
        apk = run(ADB + ["shell", "pm", "path", PACKAGE]).stdout.strip().splitlines()[0]
        if not apk.startswith("package:/data/app/"): raise RuntimeError("Unexpected probe package location.")
        libraries = str(PurePosixPath(apk.removeprefix("package:")).parent / "lib/arm64")
        home = run(ADB + ["shell", "run-as", PACKAGE, "pwd"]).stdout.strip()
        remote = home + "/files/provider-proof-" + uuid.uuid4().hex[:8]
        run(ADB + ["shell", "run-as", PACKAGE, "mkdir", "-p", remote + "/home/gh", remote + "/home/glab"])
        environment = ["env", "HOME=" + remote + "/home", "TMPDIR=" + remote,
                       "GH_CONFIG_DIR=" + remote + "/home/gh", "GLAB_CONFIG_DIR=" + remote + "/home/glab",
                       "GH_NO_UPDATE_NOTIFIER=1",
                       "PILON_GIT_CREDENTIAL_SOCKET=" + remote + "/absent.socket"]

        def command(name, *arguments, connection=None, bridge=None):
            values = environment.copy()
            if connection is not None:
                values[-1] = "PILON_GIT_CREDENTIAL_SOCKET=" + connection
            if bridge is not None:
                values.append("PILON_TEST_BRIDGE_PORT=" + str(bridge))
            program = libraries + "/libpilon_" + name.replace("-", "_") + ".so"
            # Toybox env treats '=' in an APK directory as an assignment; execute it through the shell's argv instead.
            return run(ADB + ["shell", shlex.join(["run-as", PACKAGE, *values, "/system/bin/sh", "-c",
                                                   'exec "$@"', "pilon-provider-probe", program, *arguments])])

        versions = {"gh": command("gh", "--version").stdout.splitlines()[0],
                    "glab": command("glab", "--version").stdout.splitlines()[0]}
        assert "2.103.3" in versions["gh"] and "1.128.0" in versions["glab"]
        progress = {"versions": versions, "missing_storage_fails_closed": {}, "same_uid_round_trips": {}}
        for name in ("gh", "glab"):
            failure = command(name + "-probe")
            assert "service=false, no plaintext persistence" in failure.stdout
            progress["missing_storage_fails_closed"][name] = True
            (proof / "progress.json").write_text(json.dumps(progress, indent=2) + "\n")
        for name, hostname in (("gh", "github.example"), ("glab", "gitlab.example")):
            socket = remote + "/" + name + ".socket"
            host_socket = str(Path(sockets.name) / (name + ".socket"))
            server = subprocess.Popen([str(work / "server-proof/host-build/server"), host_socket, name, hostname],
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if not select.select([server.stdout], [], [], 10)[0]:
                raise RuntimeError("Provider fixture server did not start.")
            line = server.stdout.readline().strip()
            ready = re.fullmatch(r"READY ([0-9]+)", line)
            if not ready:
                details = server.stderr.read() if server.poll() is not None else ""
                raise RuntimeError("Provider fixture server startup failed: " + line + details[-2000:])
            with host_socket_api.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                port = reservation.getsockname()[1]
            mapping = "tcp:" + str(port)
            run(ADB + ["reverse", "--no-rebind", mapping, "localfilesystem:" + host_socket])
            reverse = mapping
            success = command(name + "-probe", "success", connection=socket, bridge=port)
            assert "service=true, no plaintext persistence" in success.stdout
            progress["same_uid_round_trips"][name] = True
            (proof / "progress.json").write_text(json.dumps(progress, indent=2) + "\n")
            run(ADB + ["reverse", "--remove", reverse])
            reverse = None
            server.terminate()
            server.communicate(timeout=5)
            server = None
        result = {"versions": versions, "same_uid_socket_round_trips": True,
                  "github_login_and_refreshable_storage": True, "gitlab_credential_storage": True,
                  "missing_storage_fails_closed": True, "no_plaintext_config_tokens": True,
                  "scope": "temporary app UID clients and host Qt fixture through owned ADB reverse; production Keystore/OAuth acceptance remains separate"}
        (proof / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2))
        print("Proof:", proof / "result.json")
    finally:
        if reverse is not None: run(ADB + ["reverse", "--remove", reverse], check=False)
        if server is not None:
            server.terminate()
            server.communicate(timeout=5)
        sockets.cleanup()
        if installed: run(ADB + ["uninstall", PACKAGE], check=False)


if __name__ == "__main__":
    main()
