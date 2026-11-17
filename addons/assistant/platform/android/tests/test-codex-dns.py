#!/usr/bin/env python3
"""No external DNS: exercise the packaged resolver's CLI and numeric/localhost output."""
import ipaddress
import subprocess
import sys
import unittest

HELPER = sys.argv.pop(1)


class ResolverTests(unittest.TestCase):
    def run_helper(self, *arguments):
        return subprocess.run([HELPER, *arguments], capture_output=True, timeout=20)

    def test_rejects_invalid_arguments_without_output(self):
        for arguments in [(), ("a", "b"), ("",), ("x" * 254,), ("https://example.com",),
                          ("a\nb",), ("a b",), ("é.example",), ("../private",)]:
            with self.subTest(arguments=arguments):
                result = self.run_helper(*arguments)
                self.assertEqual(result.returncode, 64)
                self.assertEqual(result.stdout, b"")
                self.assertEqual(result.stderr, b"")

    def test_numeric_and_localhost_results_have_bounded_unique_protocol(self):
        for hostname in ["127.0.0.1", "localhost"]:
            with self.subTest(hostname=hostname):
                result = self.run_helper(hostname)
                self.assertEqual(result.returncode, 0)
                self.assertEqual(result.stderr, b"")
                self.assertLessEqual(len(result.stdout), 8192)
                self.assertTrue(result.stdout.endswith(b"\n"))
                lines = result.stdout.decode("ascii").splitlines()
                self.assertEqual(lines[0], "PILON_DNS_V1")
                self.assertTrue(1 <= len(lines[1:]) <= 64)
                self.assertEqual(len(lines[1:]), len(set(lines[1:])))
                for line in lines[1:]:
                    fields = line.split(" ")
                    address = ipaddress.ip_address(fields[1])
                    self.assertTrue(address.is_loopback)
                    self.assertEqual(str(address.version), fields[0])
                    self.assertEqual(len(fields), 2 if address.version == 4 else 3)
                    if address.version == 6:
                        self.assertTrue(0 <= int(fields[2]) <= 2**32 - 1)


if __name__ == "__main__":
    unittest.main()
