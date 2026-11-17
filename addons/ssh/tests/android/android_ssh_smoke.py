#!/usr/bin/env python3
"""Exercise Android SSH/SCP against an isolated local server, without personal keys."""
import json
import os
from pathlib import Path
import pwd
import shlex
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[4]
ADB = str(Path.home() / "Android/Sdk/platform-tools/adb")


def run(*arguments):
    result = subprocess.run(arguments, capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise RuntimeError(result.stderr[-2000:] + result.stdout[-2000:])
    return result.stdout.strip()


def main():
    work = ROOT / "build/android/network-runtime"
    work.mkdir(exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="ssh-proof-", dir=work))
    remote = "/data/local/tmp/pilon-ssh-proof-" + directory.name
    runtime = ROOT / "builds/android/runtime/arm64-v8a"
    openssl = Path(os.environ.get("PILON_ANDROID_BUILD_DIR", ROOT / "build/android/app")) / "_deps/pilon_openssl-build"
    server = None
    reverse = None
    try:
        for name in ("host", "identity", "wrong-host"):
            run("/usr/bin/ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(directory / name))
        (directory / "authorized_keys").write_text((directory / "identity.pub").read_text())
        (directory / "authorized_keys").chmod(0o600)
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        user = pwd.getpwuid(os.getuid()).pw_name
        configuration = (f"ListenAddress 127.0.0.1\nPort {port}\nHostKey {directory}/host\n"
                         f"PidFile {directory}/server.pid\nAuthorizedKeysFile {directory}/authorized_keys\n"
                         "PasswordAuthentication no\nKbdInteractiveAuthentication no\nUsePAM no\n"
                         f"StrictModes no\nAllowUsers {user}\nSubsystem sftp internal-sftp\n")
        (directory / "sshd_config").write_text(configuration)
        with (directory / "sshd.log").open("w") as log:
            server = subprocess.Popen(["/usr/bin/sshd", "-D", "-e", "-f", str(directory / "sshd_config")],
                                      stdout=log, stderr=subprocess.STDOUT)
            time.sleep(.2)
            if server.poll() is not None:
                raise RuntimeError((directory / "sshd.log").read_text())
            run(ADB, "-d", "shell", "mkdir", "-p", remote + "/home/.ssh")
            for source, target in ((runtime / "libpilon_ssh.so", "ssh"), (runtime / "libpilon_scp.so", "scp"),
                                   (openssl / "libcrypto_3.so", "libcrypto_3.so"),
                                   (openssl / "libssl_3.so", "libssl_3.so"), (directory / "identity", "identity")):
                run(ADB, "-d", "push", str(source), remote + "/" + target)
            run(ADB, "-d", "shell", f"chmod 700 {remote}/ssh {remote}/scp && chmod 600 {remote}/identity")
            run(ADB, "-d", "reverse", "--no-rebind", f"tcp:{port}", f"tcp:{port}")
            reverse = port
            environment = ["env", "HOME=" + remote + "/home", "TMPDIR=" + remote]

            def command(program, *arguments):
                return subprocess.run([ADB, "-d", "shell", shlex.join(environment + [remote + "/" + program, *arguments])],
                                      capture_output=True, text=True, timeout=30)

            version = command("ssh", "-V")
            version_text = (version.stdout + version.stderr).strip()
            assert version.returncode == 0 and "OpenSSH_10.6p2" in version_text, version_text
            options = ["-p", str(port), "-oBatchMode=yes", "-oStrictHostKeyChecking=yes", "-i", remote + "/identity"]
            destination = user + "@127.0.0.1"
            rejected = command("ssh", *options, destination, "printf should-not-run")
            assert rejected.returncode != 0 and "Host key verification failed" in rejected.stdout + rejected.stderr

            def trust(name):
                key = (directory / (name + ".pub")).read_text().split()
                (directory / "known_hosts").write_text(f"[127.0.0.1]:{port} {key[0]} {key[1]}\n")
                run(ADB, "-d", "push", str(directory / "known_hosts"), remote + "/home/.ssh/known_hosts")

            trust("host")
            authenticated = command("ssh", *options, "-oPermitLocalCommand=yes",
                                    "-oLocalCommand=printf PILON_AUTHENTICATED", destination, "printf PILON_REMOTE_OK")
            assert authenticated.returncode == 0 and "PILON_AUTHENTICATEDPILON_REMOTE_OK" in authenticated.stdout
            payload = "Pilon transfer\nHandwriting: café\n".encode()
            (directory / "payload.txt").write_bytes(payload)
            run(ADB, "-d", "push", str(directory / "payload.txt"), remote + "/upload.txt")
            scp_options = ["-S", remote + "/ssh", "-P", str(port), "-oBatchMode=yes",
                           "-oStrictHostKeyChecking=yes", "-i", remote + "/identity"]
            upload = command("scp", *scp_options, remote + "/upload.txt", destination + ":" + str(directory / "received.txt"))
            assert upload.returncode == 0, upload.stderr
            assert (directory / "received.txt").read_bytes() == payload
            download = command("scp", *scp_options, destination + ":" + str(directory / "received.txt"), remote + "/download.txt")
            assert download.returncode == 0, download.stderr
            run(ADB, "-d", "pull", remote + "/download.txt", str(directory / "download.txt"))
            assert (directory / "download.txt").read_bytes() == payload
            trust("wrong-host")
            mismatch = command("ssh", *options, destination, "printf should-not-run")
            assert mismatch.returncode != 0 and "REMOTE HOST IDENTIFICATION HAS CHANGED" in mismatch.stdout + mismatch.stderr
            proof = {"version": version_text, "unknown_host_rejected": True, "changed_host_rejected": True,
                     "authentication_marker": True, "remote_command": True, "scp_round_trip": True,
                     "scope": "isolated adb-shell helper proof; app UID integration is separate"}
            (directory / "result.json").write_text(json.dumps(proof, indent=2) + "\n")
            print(json.dumps(proof, indent=2))
            print("Proof:", directory / "result.json")
    finally:
        if reverse is not None:
            subprocess.run([ADB, "-d", "reverse", "--remove", f"tcp:{reverse}"], capture_output=True)
        subprocess.run([ADB, "-d", "shell", "rm", "-rf", remote], capture_output=True)
        if server is not None:
            server.terminate()
            server.wait(timeout=5)
        for name in ("host", "identity", "wrong-host"):
            (directory / name).unlink(missing_ok=True)


if __name__ == "__main__":
    main()
