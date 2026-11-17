#!/usr/bin/env python3
"""Stage source, Linux application bundles, or signed Android release artifacts."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile
import tomllib

from environment import load_environment


ROOT = Path(__file__).resolve().parents[2]
SOURCE_PATHS = ("README.md", "LICENSE", "COPYING", "AGENTS.md", ".gitignore", ".gitattributes", ".github", ".env.example", "docs", "src", "addons", "examples")
ADDONS = ("files", "ssh", "assistant", "notes", "calendar", "git", "devices")
SKIP_DIRECTORIES = {".git", "__pycache__", ".pytest_cache", ".venv", "venv", "node_modules", "dist", "target", ".cache", ".gradle", ".pilon", "CMakeFiles", ".qt", ".qtc"}
PRIVATE_NAMES = {"auth.json", "credentials.json", "secrets.json", "id_rsa", "id_dsa", "id_ed25519", "id_ecdsa", "release.env", "CMakeCache.txt", "cmake_install.cmake", "compile_commands.json"}
PRIVATE_SUFFIXES = {".pem", ".key", ".p12", ".pfx", ".keystore", ".jks", ".log", ".pyc", ".pyo", ".db", ".sqlite", ".sqlite3", ".pin", ".apk", ".aab", ".so", ".a", ".o"}
PRIVATE_KEY = re.compile(br"-----BEGIN [A-Z0-9 ]*PRIVATE KEY(?: BLOCK)?-----")
SOURCE_ARCHIVES = (
    ("libvterm-0.3.3.tar.gz", "09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0", "https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz", ("build/_deps/libvterm-subbuild/libvterm-populate-prefix/src/libvterm-0.3.3.tar.gz", "build/android/app/_deps/libvterm-subbuild/libvterm-populate-prefix/src/libvterm-0.3.3.tar.gz")),
    ("libical-3.0.20.tar.gz", "e73de92f5a6ce84c1b00306446b290a2b08cdf0a80988eca0a2c9d5c3510b4c2", "https://github.com/libical/libical/releases/download/v3.0.20/libical-3.0.20.tar.gz", ("build/_deps/libical-subbuild/libical-populate-prefix/src/libical-3.0.20.tar.gz", "build/android/app/_deps/libical-subbuild/libical-populate-prefix/src/libical-3.0.20.tar.gz")),
)
ANDROID_SOURCE_ARCHIVES = (
    ("git-2.56.0.tar.xz", "26c56c296b38c0695b26fa95f475f1d01704d2d38e73465ca30b0b2f5dc789d3", "https://www.kernel.org/pub/software/scm/git/git-2.56.0.tar.xz", ("build/android/git-runtime/downloads/git-2.56.0.tar.xz",)),
    ("ffmpeg-9.0.2.tar.xz", "8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e", "https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz", ("build/android/media-runtime/downloads/ffmpeg-9.0.2.tar.xz",)),
    ("openssl-3.5.9.tar.gz", "603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a", "https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz", ("build/android/app/_deps/pilon_openssl-subbuild/pilon_openssl-populate-prefix/src/openssl-3.5.9.tar.gz",)),
    ("libssh2-1.11.1.tar.gz", "d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7", "https://www.libssh2.org/download/libssh2-1.11.1.tar.gz", ("build/android/app/_deps/libssh2-subbuild/libssh2-populate-prefix/src/libssh2-1.11.1.tar.gz",)),
)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def source_files(root):
    for relative in SOURCE_PATHS:
        source = root / relative
        if not source.exists() and not source.is_symlink():
            continue
        pending = [source]
        while pending:
            path = pending.pop()
            name = path.name
            if name in SKIP_DIRECTORIES or name in PRIVATE_NAMES or (name.startswith(".env") and name != ".env.example"):
                continue
            if path.is_symlink():
                raise ValueError(f"Source archive cannot contain symbolic links: {path.relative_to(root)}")
            if path.is_dir():
                if path.relative_to(root).as_posix() == "addons/calendar/server/data":
                    continue
                if name.startswith(("build-", "cmake-build-")) or name in {"build", "builds"}:
                    continue
                pending.extend(sorted(path.iterdir(), reverse=True))
            elif path.is_file() and path.suffix.lower() not in PRIVATE_SUFFIXES and ".backup" not in name:
                if PRIVATE_KEY.search(path.read_bytes()):
                    raise ValueError(f"Private key material is forbidden in a release: {path.relative_to(root)}")
                yield path


def copy_file(source, destination):
    if source.is_symlink() or not source.is_file():
        raise ValueError(f"Expected a regular release input: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    destination.chmod(0o755 if os.access(source, os.X_OK) else 0o644)


def stage_source(root, stage):
    for source in source_files(root):
        copy_file(source, stage / source.relative_to(root))
    if not (stage / "src/CMakeLists.txt").is_file() or not (stage / "COPYING").is_file():
        raise ValueError("Source release requires src/CMakeLists.txt and COPYING")


def project_version(root):
    match = re.search(r"project\(Pilon\s+VERSION\s+([0-9]+(?:\.[0-9]+)*)\b", (root / "src/CMakeLists.txt").read_text())
    if not match:
        raise ValueError("Cannot read Pilon's project version")
    return match[1]


def write_inventory(stage, kind, native=(), corresponding=False):
    script_roots = [stage / "src/scripts", *sorted((stage / "addons").glob("*/scripts"))]
    recipes = {str(path.relative_to(stage)): digest(path) for folder in script_roots for path in sorted(folder.rglob("*.sh"))}
    notices = {str(path.relative_to(stage)): digest(path) for path in sorted(stage.rglob("*"))
               if path.is_file() and any(word in path.name.upper() for word in ("LICENSE", "LICENCE", "COPYING", "NOTICE", "SOURCE.TXT"))}
    inventory = {"format": 1, "artifact": kind, "application_source": "Included in this archive", "build_recipes_sha256": recipes,
                 "third_party_notices_sha256": notices, "native_runtimes": list(native),
                 "corresponding_source_requested": corresponding,
                 "third_party_source_requirement": "Pinned source URLs, hashes, patches and build commands are included. When --corresponding-source is used, see the separate source report and its matching archive assets for validated cached sources and any remaining gaps. Review missing source or relink requirements before binary distribution."}
    (stage / "SOURCE-INVENTORY.json").write_text(json.dumps(inventory, indent=2, sort_keys=True) + "\n")
    if kind == "linux":
        (stage / "BUNDLE.txt").write_text("Run builds/pilon from this extracted directory. Install the Linux dependencies in docs/install.md. This application bundle uses system Qt and shared libraries; it is not self-contained. Optional Python, inference and Office runtimes use the included src/scripts setup commands. Models and user data are not included. Install a launcher with python3 src/scripts/install-desktop-entry.py.\n")


def write_archive(stage, output, epoch=0):
    temporary = output.with_name(output.name + ".part")
    try:
        with temporary.open("wb") as file, gzip.GzipFile(filename="", mode="wb", fileobj=file, mtime=epoch) as compressed, tarfile.open(fileobj=compressed, mode="w|", format=tarfile.PAX_FORMAT) as archive:
            for path in sorted(stage.rglob("*")):
                if not path.is_file() or path.is_symlink():
                    continue
                info = tarfile.TarInfo(stage.name + "/" + str(path.relative_to(stage)))
                info.size = path.stat().st_size
                info.mode = 0o755 if os.access(path, os.X_OK) else 0o644
                info.mtime = epoch
                with path.open("rb") as stream:
                    archive.addfile(info, stream)
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)


def stage_linux(root, stage, binary):
    if not os.access(binary, os.X_OK):
        raise ValueError("Linux release input must be executable")
    copy_file(binary, stage / "builds/pilon")
    for addon in ADDONS:
        copy_file(root / "builds/addons" / (addon + ".piext"), stage / "builds/addons" / (addon + ".piext"))
    native = []
    for name in ("llama-server", "whisper-cli"):
        source = root / "runtime/bin" / name
        if source.exists():
            copy_file(source, stage / "runtime/bin" / name)
            native.append("runtime/bin/" + name)
    if native and (root / "runtime/llama-prefix-cache.patch").is_file():
        copy_file(root / "runtime/llama-prefix-cache.patch", stage / "runtime/llama-prefix-cache.patch")
    return native


def verify_android(apk, apksigner):
    jdk = Path(os.environ.get("JAVA_HOME") or Path.home() / "Android/jdk-21")
    environment = {**os.environ, "JAVA_HOME": str(jdk), "PATH": str(jdk / "bin") + os.pathsep + os.environ.get("PATH", "")}
    result = subprocess.run([str(apksigner), "verify", "--print-certs", str(apk)], env=environment, capture_output=True, text=True, check=True)
    if "CN=Android Debug" in result.stdout or "CN=Android Debug" in result.stderr:
        raise ValueError("Android release packaging refuses the development signing certificate")


def corresponding_sources(root, output_dir, stem, kind, application_archive):
    report = {"format": 1, "artifact": kind, "application_archive": application_archive.name,
              "application_archive_sha256": digest(application_archive), "provided": [], "missing": []}
    outputs = []

    def add_archive(path, name, expected, url, **metadata):
        row = {"component": metadata.pop("component", name), "url": url, **metadata}
        if not path.is_file() or path.is_symlink():
            report["missing"].append({**row, "reason": "Exact source archive is not cached", "expected_sha256": expected})
            return
        actual = digest(path)
        if actual != expected:
            report["missing"].append({**row, "reason": "Cached source archive does not match its pinned digest", "expected_sha256": expected, "observed_sha256": actual})
            return
        if path.stat().st_size >= 1_800_000_000:
            raise ValueError("Source archive needs separate parts before packaging: " + name)
        target = output_dir / (stem + "-source-" + name)
        temporary = target.with_name(target.name + ".part")
        copy_file(path, temporary)
        temporary.replace(target)
        outputs.append(target)
        report["provided"].append({**row, "asset": target.name, "sha256": actual, "bytes": target.stat().st_size})

    for name, expected, url, candidates in SOURCE_ARCHIVES + (ANDROID_SOURCE_ARCHIVES if kind == "android" else ()):
        path = next((root / candidate for candidate in candidates if (root / candidate).is_file()), root / candidates[0])
        add_archive(path, name, expected, url, validation="Pinned application build-recipe SHA-256")

    if kind == "linux":
        for name, expected, url in (
            ("llama-4d9176092d00586775af140581bb0b558ddc4389.tar.gz", "1a76abb8f7fe887c9cd37f71637b1a75334340e4e7fad38ef9b15d1a47ac511a", "https://codeload.github.com/ggml-org/llama.cpp/tar.gz/4d9176092d00586775af140581bb0b558ddc4389"),
            ("whisper-b4938.tar.gz", "6d8d70a014ca2b10f8a6d006b8f423e5f5ef2afcfbe92b57ab4e01107238112a", "https://codeload.github.com/ggml-org/whisper.cpp/tar.gz/refs/tags/b4938"),
        ):
            folder = "runtime" if name.startswith("llama") else "media-runtime"
            if (root / "runtime/bin" / ("llama-server" if folder == "runtime" else "whisper-cli")).is_file():
                add_archive(root / "build/android" / folder / "downloads" / name, name, expected, url, validation="Same pinned upstream source used by the Linux and Android recipes")

    if kind == "android":
        cache = root / "build/publication/sources"
        cache.mkdir(parents=True, exist_ok=True)
        for name, checkout, commit, url in (
            ("qtbase", root / "build/android/qt-platform/upstream", "025bdad181de241e81bf853c8a2d7bf3d19261a9", "https://github.com/qt/qtbase"),
            ("nucleo", root / "build/android/codex/cargo/git/checkouts/nucleo-425d994cd74b3654/4253de9", "4253de9faabb4e5c6d81d946a5e35a90f87347ee", "https://github.com/helix-editor/nucleo"),
        ):
            archive = cache / (name + "-" + commit + ".tar.gz")
            try:
                actual = subprocess.check_output(["git", "-C", str(checkout), "rev-parse", "HEAD"], text=True).strip()
                if actual != commit:
                    raise ValueError("Cached checkout differs from the frozen source commit")
                subprocess.run(["git", "-C", str(checkout), "archive", "--format=tar.gz", "--prefix=" + name + "-" + commit + "/", "--output=" + str(archive), commit], check=True)
                add_archive(archive, archive.name, digest(archive), url + "/tree/" + commit, component=name, commit=commit, validation="Git object identity matches the frozen upstream commit; archive contains that immutable tree")
            except (OSError, ValueError, subprocess.CalledProcessError):
                report["missing"].append({"component": name, "url": url + "/tree/" + commit, "reason": "Cached upstream Git checkout is unavailable or does not match the frozen commit"})

        glab = root / "build/android/git-providers/downloads/glab-1.128.0-source.tar.gz"
        add_archive(glab, glab.name, "908296f5aae4d63bb942f6da57910774fb67df79cfe0167caa0d8cb73e7e6a40", "https://gitlab.com/api/v4/projects/gitlab-org%2Fcli/packages/generic/glab/1%2E128%2E0/glab_1%2E128%2E0_source%2Etar%2Egz", component="Go provider source including vendored D2 and Hashicorp MPL modules", validation="Pinned application build-recipe SHA-256; vendored source also supplies the same Hashicorp go-version version used by GitHub CLI")
        metadata = root / "build/android/codex/dependencies.json"
        lock = root / "build/android/codex/source/codex-rust-v0.167.0/codex-rs/Cargo.lock"
        if metadata.is_file() and lock.is_file():
            locked = {(item["name"], item["version"]): item.get("checksum") for item in tomllib.loads(lock.read_text())["package"] if item.get("checksum")}
            for item in json.loads(metadata.read_text())["packages"]:
                if not re.search(r"(?:GPL|MPL|EPL|CDDL)", item.get("license") or "") or not (item.get("source") or "").startswith("registry+"):
                    continue
                source = Path(item["manifest_path"]).parent
                name = item["name"] + "-" + item["version"] + ".crate"
                archive = source.parents[2] / "cache" / source.parent.name / name
                expected = locked.get((item["name"], item["version"]))
                if expected:
                    add_archive(archive, name, expected, "https://crates.io/api/v1/crates/" + item["name"] + "/" + item["version"] + "/download", license=item["license"], validation="Frozen Cargo.lock checksum; conservative superset selected from dependency metadata")
        else:
            report["missing"].append({"component": "Codex copyleft dependency sources", "reason": "Frozen Cargo.lock or dependency metadata is unavailable"})

        provenance = cache / "provenance.json"
        evidence_files = set()
        evidence_checksums = {}
        coverage = set()
        if provenance.is_file():
            data = json.loads(provenance.read_text())
            for row in data.get("archives", []):
                name = row["filename"]
                if Path(name).name != name:
                    raise ValueError("Source provenance filenames must be basenames")
                metadata = {key: value for key, value in row.items() if key not in {"filename", "sha256", "url"}}
                add_archive(cache / name, name, row["sha256"], row["url"], **metadata)
                coverage.add(row.get("component"))
                coverage.update(row.get("covers", []))
                evidence_files.update(row.get("evidence_files", []))
                evidence_checksums.update(row.get("evidence_checksums", {}))
            report["missing"].extend(data.get("missing", []))
        else:
            report["missing"].append({"component": "Qt module, Qt FFmpeg and LibreOffice sources", "reason": "Publication source-download provenance has not been prepared"})
        for component in ("qtdeclarative", "qtmultimedia", "qtsvg", "qtimageformats", "FFmpeg-Qt", "LibreOffice-Android", "onnxruntime-eigen", "litert-eigen"):
            if component not in coverage:
                report["missing"].append({"component": component, "reason": "The matching upstream source archive and release build-metadata pin have not been supplied"})
        kit = Path(os.environ.get("PILON_ANDROID_QT", str(Path.home() / "Qt/6.12.0/android_arm64_v8a")))
        report["qt_sbom_modules"] = []
        with tempfile.TemporaryDirectory(prefix="pilon-corresponding-source-") as directory:
            stage = Path(directory) / (stem + "-corresponding-source")
            stage.mkdir()
            for source in sorted((kit / "sbom").glob("*.spdx.json")):
                copy_file(source, stage / "qt-sdk-sbom" / source.name)
                report["qt_sbom_modules"].append(source.name)
            for name in sorted(evidence_files):
                if Path(name).name != name:
                    raise ValueError("Source evidence filenames must be basenames")
                source = cache / name
                if not evidence_checksums.get(name) or digest(source) != evidence_checksums[name]:
                    raise ValueError("Source evidence does not match its recorded digest: " + name)
                if PRIVATE_KEY.search(source.read_bytes()):
                    raise ValueError("Private key material is forbidden in source evidence")
                copy_file(source, stage / "source-provenance" / name)
            report["source_evidence_sha256"] = evidence_checksums
            if provenance.is_file():
                copy_file(provenance, stage / "source-provenance/provenance.json")
            report_path = output_dir / (stem + "-corresponding-source-report.json")
            report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
            copy_file(report_path, stage / report_path.name)
            material = output_dir / (stage.name + ".tar.gz")
            write_archive(stage, material, int(os.environ.get("SOURCE_DATE_EPOCH", "0")))
            outputs.extend((material, report_path))
    else:
        report_path = output_dir / (stem + "-corresponding-source-report.json")
        report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        outputs.append(report_path)
    return outputs


def package(args, root=ROOT):
    version = project_version(root)
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    stem = f"pilon-{version}-{args.kind}"
    if args.kind == "linux":
        stem += "-" + platform.machine()
    if args.kind == "android":
        stem += "-arm64-v8a"
        if not args.android_apk:
            raise ValueError("Android release packaging requires --android-apk")
        verify_android(args.android_apk, args.apksigner)
    outputs = []
    with tempfile.TemporaryDirectory(prefix="pilon-release-") as directory:
        stage = Path(directory) / (stem + ("-source-notices" if args.kind == "android" else ""))
        stage.mkdir()
        stage_source(root, stage)
        native = stage_linux(root, stage, args.linux_binary) if args.kind == "linux" else []
        if args.kind == "android":
            licenses = root / "builds/android/licenses"
            if not licenses.is_dir() or not any(licenses.rglob("*LICENSE*")):
                raise ValueError("Android release requires the staged third-party notices")
            for path in sorted(licenses.rglob("*")):
                if path.is_symlink():
                    raise ValueError("Android notices cannot contain symbolic links")
                if path.is_file():
                    copy_file(path, stage / "third-party-notices" / path.relative_to(licenses))
            apk = output_dir / (stem + ".apk")
            copy_file(args.android_apk, apk.with_suffix(".apk.part"))
            apk.with_suffix(".apk.part").replace(apk)
            (stage / "APK-SHA256SUMS").write_text(digest(apk) + "  " + apk.name + "\n")
            outputs.append(apk)
        write_inventory(stage, args.kind, native, getattr(args, "corresponding_source", False))
        archive = output_dir / (stage.name + ".tar.gz")
        write_archive(stage, archive, int(os.environ.get("SOURCE_DATE_EPOCH", "0")))
        outputs.append(archive)
    if getattr(args, "corresponding_source", False):
        outputs.extend(corresponding_sources(root, output_dir, stem, args.kind, archive))
    checksums = output_dir / (stem + "-SHA256SUMS")
    checksums.write_text("".join(digest(path) + "  " + path.name + "\n" for path in outputs))
    return outputs + [checksums]


def main():
    load_environment()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=("source", "linux", "android"))
    parser.add_argument("--output-dir", type=Path, default=ROOT / "builds/releases")
    parser.add_argument("--linux-binary", type=Path, default=ROOT / "builds/pilon")
    parser.add_argument("--android-apk", type=Path)
    parser.add_argument("--corresponding-source", action="store_true", help="Also export validated cached upstream source assets and a report of missing material")
    sdk = Path(os.environ.get("ANDROID_SDK_ROOT", str(Path.home() / "Android/Sdk")))
    parser.add_argument("--apksigner", type=Path, default=sdk / "build-tools/37.0.0/apksigner")
    args = parser.parse_args()
    try:
        for path in package(args):
            print(path)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
