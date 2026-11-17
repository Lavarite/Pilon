#!/usr/bin/env python3
"""Prove native Android Git commits/transfers/TLS in an isolated USB-device folder."""
import http.server
import json
from pathlib import Path
import shlex
import ssl
import subprocess
import tempfile
import threading
import uuid

ROOT = Path(__file__).resolve().parents[4]
ADB = [str(Path.home() / "Android/Sdk/platform-tools/adb"), "-d"]


def run(arguments, check=True, binary=False):
    result = subprocess.run(arguments, capture_output=True, text=not binary, timeout=60)
    if check and result.returncode:
        raise RuntimeError(str(result.stderr)[-2000:] + str(result.stdout)[-2000:])
    return result


def main():
    work = ROOT / "build/android/git-runtime"
    directory = Path(tempfile.mkdtemp(prefix="git-proof-", dir=work))
    remote = "/data/local/tmp/pilon-git-proof-" + uuid.uuid4().hex
    runtime = ROOT / "builds/android/runtime/arm64-v8a"
    openssl = ROOT / "build/android/app/_deps/pilon_openssl-build"
    server = None
    reverse = None
    try:
        run(ADB + ["shell", "mkdir", "-p", remote + "/home", remote + "/seed"])
        for source in (runtime / "libpilon_git.so", runtime / "libpilon_git_remote_http.so",
                       openssl / "libcrypto_3.so", openssl / "libssl_3.so"):
            run(ADB + ["push", str(source), remote + "/" + source.name])
        run(ADB + ["shell", "chmod", "700", remote + "/libpilon_git.so", remote + "/libpilon_git_remote_http.so"])
        # The phone's trusted system anchors are public; convert their old-hash directory to a PEM bundle.
        anchors = run(ADB + ["exec-out", "sh", "-c", "cat /apex/com.android.conscrypt/cacerts/*"], binary=True).stdout
        assert 100 < anchors.count(b"-----BEGIN CERTIFICATE-----") < 2000
        assert len(anchors) < 16 * 1024 * 1024
        (directory / "platform-ca.pem").write_bytes(anchors)
        run(ADB + ["push", str(directory / "platform-ca.pem"), remote + "/platform-ca.pem"])

        def git(*arguments, check=True):
            command = ["env", "HOME=" + remote + "/home", "TMPDIR=" + remote,
                       "GIT_EXEC_PATH=" + remote, "GIT_CONFIG_NOSYSTEM=1",
                       "GIT_SSL_CAINFO=" + remote + "/platform-ca.pem", remote + "/libpilon_git.so",
                       *arguments]
            return run(ADB + ["shell", shlex.join(command)], check=check)

        version = git("--version").stdout.strip()
        assert version == "git version 2.56.0"
        git("-C", remote + "/seed", "-c", "init.defaultBranch=main", "init")
        git("-C", remote + "/seed", "config", "user.name", "Pilon Test")
        git("-C", remote + "/seed", "config", "user.email", "pilon-test@localhost")
        payload = '{"text":"Handwriting café","strokes":[[1,2],[3,4]]}\n'.encode()
        (directory / "note.pin").write_bytes(payload)
        run(ADB + ["push", str(directory / "note.pin"), remote + "/seed/note.pin"])
        git("-C", remote + "/seed", "add", "--", "note.pin")
        git("-C", remote + "/seed", "commit", "-m", "Preserve handwriting café")
        assert not git("-C", remote + "/seed", "status", "--porcelain").stdout.strip()
        assert "café" in git("-C", remote + "/seed", "log", "-1", "--format=%s").stdout
        git("clone", "--bare", "--no-local", remote + "/seed", remote + "/remote.git")
        git("clone", "--no-local", remote + "/remote.git", remote + "/clone")
        git("-C", remote + "/clone", "config", "user.name", "Pilon Test")
        git("-C", remote + "/clone", "config", "user.email", "pilon-test@localhost")
        git("-C", remote + "/clone", "commit", "--allow-empty", "-m", "Round trip")
        git("-C", remote + "/clone", "push", "origin", "main")
        assert git("--git-dir=" + remote + "/remote.git", "log", "-1", "--format=%s").stdout.strip() == "Round trip"
        git("--git-dir=" + remote + "/remote.git", "fsck", "--strict")
        https = git("ls-remote", "--exit-code", "https://github.com/git/git.git", "refs/heads/master")
        assert "refs/heads/master" in https.stdout

        run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
             "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
             "-keyout", str(directory / "test-key.pem"), "-out", str(directory / "test-cert.pem")])

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_error(500)

            def log_message(self, *_):
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(directory / "test-cert.pem", directory / "test-key.pem")
        server.socket = context.wrap_socket(server.socket, server_side=True)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        port = server.server_port
        run(ADB + ["reverse", "--no-rebind", f"tcp:{port}", f"tcp:{port}"])
        reverse = port
        rejected = git("ls-remote", f"https://127.0.0.1:{port}/fixture.git", check=False)
        assert rejected.returncode != 0 and "certificate" in rejected.stderr.lower(), rejected.stderr
        proof = {"version": version, "unicode_commit": True, "clean_status": True,
                 "clone_push_round_trip": True, "fsck": True, "https_platform_trust": True,
                 "untrusted_tls_rejected": True, "scope": "isolated adb-shell; app UID/UI integration remains separate"}
        (directory / "result.json").write_text(json.dumps(proof, indent=2) + "\n")
        print(json.dumps(proof, indent=2))
        print("Proof:", directory / "result.json")
    finally:
        if reverse is not None:
            run(ADB + ["reverse", "--remove", f"tcp:{reverse}"], check=False)
        if server is not None:
            server.shutdown()
            server.server_close()
        (directory / "test-key.pem").unlink(missing_ok=True)
        run(ADB + ["shell", "rm", "-rf", "--", remote], check=False)


if __name__ == "__main__":
    main()
