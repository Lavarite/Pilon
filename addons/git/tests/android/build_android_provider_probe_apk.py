#!/usr/bin/env python3
"""Package fixture-only native probes without altering or launching the Pilon app."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[4]


def main():
    sdk = Path(os.environ.get("ANDROID_SDK_ROOT", Path.home() / "Android/Sdk"))
    java = Path(os.environ.get("JAVA_HOME", Path.home() / "Android/jdk-21"))
    tools = sdk / "build-tools/37.0.0"
    work = ROOT / "build/android/git-providers/probe-apk"
    work.mkdir(exist_ok=True)
    archive = work / "unsigned.apk"
    subprocess.run([str(tools / "aapt2"), "link", "--manifest",
                    str(ROOT / "addons/git/tests/android/android_provider_probe_manifest.xml"),
                    "-I", str(sdk / "platforms/android-37.0/android.jar"),
                    "--version-code", "1", "--version-name", "1", "-o", str(archive)], check=True)
    runtime = ROOT / "builds/android/runtime/arm64-v8a"
    files = {"gh": runtime / "libpilon_gh.so", "glab": runtime / "libpilon_glab.so",
             "gh_probe": work.parent / "gh-storage-probe", "glab_probe": work.parent / "glab-storage-probe"}
    with zipfile.ZipFile(archive, "a", compression=zipfile.ZIP_DEFLATED) as package:
        for name, source in files.items():
            package.write(source, "lib/arm64-v8a/libpilon_" + name + ".so")
        licenses = ROOT / "builds/android/licenses/git-providers"
        for source in licenses.rglob("*"):
            if source.is_file(): package.write(source, "assets/licenses/" + source.relative_to(licenses).as_posix())
    aligned = work / "aligned.apk"
    subprocess.run([str(tools / "zipalign"), "-f", "-P", "16", "4", str(archive), str(aligned)], check=True)
    environment = {**os.environ, "JAVA_HOME": str(java), "PATH": str(java / "bin") + ":" + os.environ["PATH"]}
    output = work / "provider-probe.apk"
    subprocess.run([str(tools / "apksigner"), "sign", "--ks", str(Path.home() / ".android/debug.keystore"),
                    "--ks-pass", "pass:android", "--key-pass", "pass:android", "--out", str(output), str(aligned)],
                   check=True, env=environment)
    subprocess.run([str(tools / "apksigner"), "verify", str(output)], check=True, env=environment)
    result = {"apk": str(output), "sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
              "package": "org.pilon.PilonProviderProbe", "launchable": False,
              "native_sources": {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in files.items()}}
    (work / "manifest.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: result[key] for key in ("apk", "sha256", "package", "launchable")}, indent=2))


if __name__ == "__main__":
    main()
