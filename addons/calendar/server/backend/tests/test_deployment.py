"""Deployment generation is tested without running privileged commands."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("calendar_install", Path(__file__).resolve().parents[2] / "install-on-vm.py")
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class DeploymentTests(unittest.TestCase):
    def test_nginx_uses_the_requested_domain_and_certificate(self):
        config = installer.nginx("calendar.example.org", True)
        self.assertIn("server_name calendar.example.org;", config)
        self.assertIn("/etc/letsencrypt/live/calendar.example.org/fullchain.pem", config)
        self.assertIn("proxy_pass http://127.0.0.1:20005;", config)
        self.assertIn("access_log off;", config)
        self.assertNotIn("vasylevskyi", config)

    def test_domain_validation_rejects_nginx_injection_and_urls(self):
        for domain in ("https://calendar.example.org", "example.org;", "example.org\n}", "-example.org", "localhost"):
            with self.subTest(domain=domain), self.assertRaises(ValueError):
                installer.validate_domain(domain)
        self.assertEqual(installer.validate_domain("Calendar.EXAMPLE.org"), "calendar.example.org")

    def test_standalone_compose_needs_no_parent_stack(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "calendar"
            root.mkdir()
            command = installer.compose_command(root, integrate=False)
            self.assertIn(str(root / "docker-compose.yml"), command)
            self.assertFalse((root.parent / "docker-compose.yml").exists())

    def test_integrated_compose_targets_the_existing_parent_project(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "PilonCalendar"
            root.mkdir()
            parent = root.parent / "docker-compose.yml"
            parent.write_text("name: existing-project\ninclude:\n  - Other/docker-compose.yml\nnetworks:\n  default: {}\n")
            installer.integrate_compose(root)
            installer.integrate_compose(root)
            self.assertEqual(parent.read_text().count("PilonCalendar/docker-compose.yml"), 1)
            self.assertIn("  - PilonCalendar/docker-compose.yml\nnetworks:\n", parent.read_text())
            self.assertIn("Other/docker-compose.yml", parent.read_text())
            self.assertIn(str(parent), installer.compose_command(root, integrate=True))

    def test_failed_nginx_reload_restores_previous_site(self):
        with tempfile.TemporaryDirectory() as directory:
            site = Path(directory) / "site.conf"
            enabled = Path(directory) / "enabled.conf"
            site.write_bytes(b"previous site\n")
            enabled.symlink_to(site)
            with patch.multiple(installer, SITE=site, ENABLED=enabled), patch.object(installer, "run") as run:
                run.side_effect = [None, subprocess.CalledProcessError(1, "systemctl")]
                with self.assertRaises(subprocess.CalledProcessError):
                    installer.apply_site("calendar.example.org", True)
            self.assertEqual(site.read_bytes(), b"previous site\n")
            self.assertEqual(enabled.readlink(), site)
