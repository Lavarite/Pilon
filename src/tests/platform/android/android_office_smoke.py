#!/usr/bin/env python3
"""Run the preserved Office app-UID test APK against an explicitly pinned app."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "src/scripts"))
from environment import development_root

DEV = development_root()
ADB = Path.home() / "Android/Sdk/platform-tools/adb"
TEST_SHA = "ddf8402acc007be20deee24235b49042b703b052ebf8eb63644cc50be1eeb942"
PACKAGE = "org.pilon.Pilon"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def adb(*arguments, **kwargs):
    return subprocess.run([str(ADB), "-d", *map(str, arguments)], check=True, capture_output=True, text=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app_sha256", help="Expected SHA-256 of the already installed main APK")
    parser.add_argument("proof", type=Path)
    args = parser.parse_args()
    args.proof.mkdir(parents=True, exist_ok=True)
    test = Path(os.environ.get("PILON_ANDROID_BUILD_DIR") or DEV / "builds/work/android/app") / "office-probe-test.apk"
    fixtures = ROOT / "src/platform/android/tests/assets/office-fixtures.zip"
    assert len(args.app_sha256) == 64 and digest(test) == TEST_SHA
    with ZipFile(test) as archive:
        assert archive.read("assets/office-fixtures.zip") == fixtures.read_bytes()
    with ZipFile(fixtures) as archive:
        manifest = json.loads(archive.read("manifest.json"))
        sources = {entry["filename"]: hashlib.sha256(archive.read(entry["filename"])).hexdigest() for entry in manifest}
    assert len(manifest) == 15
    locations = adb("shell", "pm", "path", PACKAGE).stdout.splitlines()
    base = next(line.removeprefix("package:") for line in locations if line.endswith("/base.apk"))

    def installed_hash():
        return adb("shell", "sha256sum", base).stdout.split()[0]

    def owned_cache():
        result = adb("shell", "run-as", PACKAGE, "find", "cache", "-maxdepth", "1", "-type", "d", "-name", "pilon-office-probe-*")
        return set(result.stdout.splitlines())

    proof = {"targetApkSha256": installed_hash(), "testApkSha256": TEST_SHA,
        "fixtureZipSha256": digest(fixtures), "fixtureSourceSha256": sources,
        "scope": "installed app UID Office JNI/service conversion; actual note import UI remains separate"}
    assert proof["targetApkSha256"] == args.app_sha256, "Installed main APK differs from the authorized checkpoint"
    before = owned_cache()
    try:
        install = adb("install", "-r", "-t", test, timeout=60)
        (args.proof / "test-install.log").write_text(install.stdout + install.stderr)
        started = time.monotonic()
        result = adb("shell", "am", "instrument", "-w", "-r", PACKAGE + ".test/org.pilon.OfficeProbeInstrumentation", timeout=240)
        proof["seconds"] = time.monotonic() - started
        output = result.stdout + result.stderr
        (args.proof / "instrumentation.log").write_text(output)
        line = next(line for line in output.splitlines() if line.startswith("INSTRUMENTATION_RESULT: facts="))
        proof["formats"] = json.loads(line.split("facts=", 1)[1])
        count = next((line for line in output.splitlines() if line.startswith("INSTRUMENTATION_RESULT: checks=")), None)
        if count is not None: proof["checks"] = int(count.split("checks=", 1)[1])
        proof["suitePassed"] = "Pilon Office checks passed:" in output and "INSTRUMENTATION_CODE: -1" in output
        assert len(proof["formats"]) == 15, "The full format matrix did not run"
        for expected, actual in zip(manifest, proof["formats"]):
            assert actual["filename"] == expected["filename"] and not actual["error"], actual
            assert actual["document"]["pages"] == expected["pages"], actual
            for text in expected["contains"]:
                assert text in actual["document"]["text"], actual
            for text in expected.get("notContains", []):
                assert text not in actual["document"]["text"], actual
        assert proof["suitePassed"], "Cancellation/source preservation/restart/failure checks did not all pass"
    finally:
        proof["ownedFixtureFoldersCleaned"] = owned_cache() == before
        proof["mainApkUnchanged"] = installed_hash() == args.app_sha256
        adb("shell", "am", "force-stop", PACKAGE)
        adb("shell", "am", "force-stop", PACKAGE + ".test")
        (args.proof / "proof.json").write_text(json.dumps(proof, indent=2) + "\n")
    assert proof["ownedFixtureFoldersCleaned"] and proof["mainApkUnchanged"]
    print("Office app-UID checks passed for 15 formats, source integrity, cancellation, process restart and invalid input")
    print(args.proof / "proof.json")


if __name__ == "__main__":
    main()
