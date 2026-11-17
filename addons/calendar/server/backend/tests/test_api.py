"""Exercise the public sharing contract against an isolated local server."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from test_calendar import publication


class SharingApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.socket = socket.socket()
        cls.socket.bind(("127.0.0.1", 0))
        cls.socket.listen(128)
        cls.base = f"http://127.0.0.1:{cls.socket.getsockname()[1]}"
        cls.key = "test-only-publisher-key-" + "k" * 32
        env = dict(os.environ, PILON_PUBLISHER_KEY=cls.key,
                   PILON_SHARE_DATABASE=str(Path(cls.directory.name) / "calendar.sqlite"))
        cls.server = subprocess.Popen(
            [sys.executable, "-m", "uvicorn", "app.main:app", "--fd", str(cls.socket.fileno()),
             "--no-access-log", "--log-level", "error"],
            cwd=Path(__file__).resolve().parents[1], env=env,
            pass_fds=(cls.socket.fileno(),), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            try:
                with urlopen(cls.base + "/api/health", timeout=.1):
                    return
            except OSError:
                if cls.server.poll() is not None:
                    break
                time.sleep(.025)
        cls.tearDownClass()
        raise RuntimeError("Isolated sharing server did not start")

    @classmethod
    def tearDownClass(cls):
        cls.server.terminate()
        cls.server.wait(timeout=10)
        cls.socket.close()
        cls.directory.cleanup()

    def request(self, method, path, payload=None, authenticated=True):
        headers = {"Content-Type": "application/json"}
        if authenticated:
            headers["Authorization"] = "Bearer " + self.key
        request = Request(self.base + path, method=method, headers=headers,
                          data=None if payload is None else json.dumps(payload).encode())
        try:
            response = urlopen(request, timeout=5)
        except HTTPError as error:
            response = error
        with response:
            return response.status, json.load(response)

    def test_explicit_creation_is_authenticated_idempotent_and_conflict_safe(self):
        token = "b" * 43
        path = f"/api/shares/{token}?create=true"
        self.assertEqual(self.request("PUT", path, publication(), authenticated=False)[0], 401)
        status, result = self.request("PUT", path, publication())
        self.assertIn(status, (200, 201))
        self.assertEqual(result["id"], token)
        self.assertIn(self.request("PUT", path, publication())[0], (200, 201))
        changed = publication()
        changed["events"][0]["title"] = "Changed intent"
        self.assertEqual(self.request("PUT", path, changed)[0], 409)
        status, result = self.request("GET", f"/api/calendars/{token}?start=2026-10-01&end=2026-11-01")
        self.assertEqual(status, 200)
        self.assertEqual(result["events"][0]["title"], "Lecture")
        self.assertTrue(result["publishedAt"])

    def test_regular_update_cannot_create_a_missing_link(self):
        self.assertEqual(self.request("PUT", "/api/shares/" + "c" * 43, publication())[0], 404)

    def test_explicit_creation_rejects_invalid_token(self):
        self.assertEqual(self.request("PUT", "/api/shares/short?create=true", publication())[0], 422)
