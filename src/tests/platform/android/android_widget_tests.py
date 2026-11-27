#!/usr/bin/env python3
"""Run the Android widget reader and schedule checks against the shared contract."""
import argparse
import gzip
import hashlib
import io
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile
import urllib.request


LIBCORE = "4ce63484144d6e245ad55e7a0027ea075cbd804c"
JSON_ARCHIVE = f"https://android.googlesource.com/platform/libcore/+archive/{LIBCORE}/json/src/main/java/org/json.tar.gz"
# Gitiles gives each tar download fresh member timestamps.
JSON_SOURCES = {
    "JSON.java": "a2cc5e4bf64bfe3edd73c6a14409f3200640454051d9947ee79f16a2396a19b1",
    "JSONArray.java": "24f10dbe57a9228b8aec8a891355c26285e7fc4e070a2f826a080300e3ca4180",
    "JSONException.java": "26e10aae9c209dabbc8f15b4156fe9fdc87f5d9d2cb2d2f078b475a61ba0df7d",
    "JSONObject.java": "b7770e43fdad8415e8f5c37a7dc06a7c1ffb3a16c557887ded1479f3116c868f",
    "JSONStringer.java": "8772ac80610cd62e906330152b09864c471a0921f199f4b9d670d98050e3d86c",
    "JSONTokener.java": "744c9ffb50c2472d7269c8d64a7d8331ea1a1691fb6536619ac9590e9ebbbd4b",
}


def libcore_json_sources(work, archive_path):
    compressed_limit, unpacked_limit = 128 * 1024, 512 * 1024
    with archive_path.open("rb") if archive_path else urllib.request.urlopen(JSON_ARCHIVE, timeout=30) as source:
        archive_bytes = source.read(compressed_limit + 1)
    if len(archive_bytes) > compressed_limit:
        raise ValueError("Android libcore JSON archive exceeds the download size limit")
    with gzip.GzipFile(fileobj=io.BytesIO(archive_bytes)) as source:
        tar_bytes = source.read(unpacked_limit + 1)
    if len(tar_bytes) > unpacked_limit:
        raise ValueError("Android libcore JSON archive exceeds the unpacked size limit")

    verified = {}
    with tarfile.open(fileobj=io.BytesIO(tar_bytes), mode="r:") as archive:
        for member in archive:
            if member.name not in JSON_SOURCES:
                continue
            if not member.isfile() or member.name in verified:
                raise ValueError(f"Ambiguous Android libcore JSON source: {member.name}")
            with archive.extractfile(member) as source:
                java = source.read()
            if hashlib.sha256(java).hexdigest() != JSON_SOURCES[member.name]:
                raise ValueError(f"Android libcore JSON source checksum does not match: {member.name}")
            verified[member.name] = java.decode("utf-8")
    if verified.keys() != JSON_SOURCES.keys():
        raise ValueError("Android libcore JSON archive is missing pinned sources")

    for name, java in verified.items():
        # These verified sources use Android's build-only annotations.
        java = re.sub(r"^import (?:static )?(?:android\.|libcore\.util\.).*;\n", "", java, flags=re.MULTILINE)
        java = re.sub(r"@(?:NonNull|Nullable|UnsupportedAppUsage)\b\s*|@SystemApi\(client = MODULE_LIBRARIES\)\s*", "", java)
        (work / name).write_text(java, encoding="utf-8")
    return [work / name for name in JSON_SOURCES]


def verify(root, jdk, archive_path=None):
    platform = root / "src/platform/android"
    sources = [platform / "src/org/pilon" / name for name in ("PilonWidgetData.java", "PilonWidgetSchedule.java")]
    sources += [platform / "host-tests/org/pilon/PilonWidgetFiles.java", platform / "tests/org/pilon/PilonWidgetDataChecks.java"]
    contract = root / "src/tests/fixtures/widgets/contract-v1.json"
    with tempfile.TemporaryDirectory(prefix="pilon-widget-tests-") as directory:
        work = Path(directory)
        sources += libcore_json_sources(work, archive_path)
        subprocess.run([jdk / "bin/javac", "--release", "17", "-d", work, *sources], check=True)
        subprocess.run([jdk / "bin/java", "-cp", work, "org.pilon.PilonWidgetDataChecks", contract], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jdk", required=True, type=Path)
    parser.add_argument("--json-archive", type=Path, help="Use a local copy of the pinned libcore JSON source archive")
    args = parser.parse_args()
    verify(Path(__file__).resolve().parents[4], args.jdk, args.json_archive)
