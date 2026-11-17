#!/usr/bin/env python3
"""Exercise the credential helper framing and failure limits without real credentials."""
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[3]


class CredentialTransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="pilon-git-credential-")
        cls.binary = str(Path(cls.directory.name) / "helper")
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                        str(ROOT / "addons/git/platform/android/gitcredential.c"), "-o", cls.binary], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def call(self, action="get", data=b"protocol=https\nhost=fixture.invalid\n\n", reply=b"\1\0\0\0\0"):
        path = str(Path(self.directory.name) / "socket")
        server = socket.socket(socket.AF_UNIX)
        server.bind(path)
        server.listen(1)
        server.settimeout(2)
        received = []

        def respond():
            with server:
                try:
                    client, _ = server.accept()
                    with client:
                        def read(size):
                            output = b""
                            while len(output) < size:
                                part = client.recv(size - len(output))
                                if not part:
                                    raise EOFError()
                                output += part
                            return output
                        length, = struct.unpack("!I", read(4))
                        received.append(read(length))
                        client.sendall(reply)
                except (socket.timeout, EOFError, BrokenPipeError):
                    pass

        thread = threading.Thread(target=respond)
        thread.start()
        result = subprocess.run([self.binary, action], input=data, capture_output=True,
                                env={**os.environ, "PILON_GIT_CREDENTIAL_SOCKET": path}, timeout=7)
        thread.join(3)
        server.close()
        Path(path).unlink()
        return result, received

    def test_private_round_trip_and_protocol_output(self):
        output = "username=fixture\npassword=café\n".encode()
        result, received = self.call(reply=b"\1" + struct.pack("!I", len(output)) + output)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, output)
        self.assertEqual(result.stderr, b"")
        self.assertEqual(received, [b"get\nprotocol=https\nhost=fixture.invalid\n\n"])

    def test_rejects_oversized_or_truncated_responses_without_output(self):
        for reply in (b"\1" + struct.pack("!I", 65537), b"\1\0\0\0\5x", b"\0\0\0\0\0"):
            with self.subTest(reply=reply):
                result, _ = self.call(reply=reply)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b"")
                self.assertNotIn(b"fixture.invalid", result.stderr)

    def test_rejects_oversized_input_and_unknown_actions(self):
        for action, data in (("get", b"x" * 65537), ("unknown", b"secret-fixture")):
            result, received = self.call(action=action, data=data)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b"")
            self.assertFalse(received)
            self.assertNotIn(b"secret-fixture", result.stderr)


if __name__ == "__main__":
    unittest.main()
