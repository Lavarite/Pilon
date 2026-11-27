#!/usr/bin/env python3
"""Stage source, Linux application bundles, or signed Android release artifacts."""
import argparse
import fcntl
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
from zipfile import ZipFile

from environment import development_root, installation_root, load_environment


ROOT = Path(__file__).resolve().parents[2]
SOURCE_MANIFEST = "SOURCE-FILES.txt"
SOURCE_PATHS = ("README.md", "CONTRIBUTING.md", "LICENSE", "COPYING", "AGENTS.md", ".gitignore", ".gitattributes", ".github", ".env.example", "docs", "src", "addons", "examples")
ADDONS = ("files", "ssh", "assistant", "notes", "calendar", "devices")
ANDROID_NOTICE_COMPONENTS = (
    "agent-runtime", "breeze", "media", "ocr", "office", "pdfium",
    "qt-platform", "runtime", "ssh", "tailscale",
)
SKIP_DIRECTORIES = {".git", "__pycache__", ".pytest_cache", ".venv", "venv", "node_modules", "dist", "target", ".cache", ".gradle", ".pilon", "CMakeFiles", ".qt", ".qtc"}
PRIVATE_NAMES = {"auth.json", "credentials.json", "secrets.json", "id_rsa", "id_dsa", "id_ed25519", "id_ecdsa", "release.env", "CMakeCache.txt", "cmake_install.cmake", "compile_commands.json"}
PRIVATE_SUFFIXES = {".pem", ".key", ".p12", ".pfx", ".keystore", ".jks", ".log", ".pyc", ".pyo", ".db", ".sqlite", ".sqlite3", ".pin", ".apk", ".aab", ".so", ".a", ".o"}
PRIVATE_KEY = re.compile(br"-----BEGIN [A-Z0-9 ]{0,64}PRIVATE KEY(?: BLOCK)?-----")
SOURCE_ARCHIVES = (
    ("libvterm-0.3.3.tar.gz", "09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0", "https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz", ("builds/work/linux/_deps/libvterm-subbuild/libvterm-populate-prefix/src/libvterm-0.3.3.tar.gz", "builds/work/android/app/_deps/libvterm-subbuild/libvterm-populate-prefix/src/libvterm-0.3.3.tar.gz")),
    ("libical-3.0.20.tar.gz", "e73de92f5a6ce84c1b00306446b290a2b08cdf0a80988eca0a2c9d5c3510b4c2", "https://github.com/libical/libical/releases/download/v3.0.20/libical-3.0.20.tar.gz", ("builds/work/linux/_deps/libical-subbuild/libical-populate-prefix/src/libical-3.0.20.tar.gz", "builds/work/android/app/_deps/libical-subbuild/libical-populate-prefix/src/libical-3.0.20.tar.gz")),
)
ANDROID_SOURCE_ARCHIVES = (
    ("ffmpeg-9.0.2.tar.xz", "8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e", "https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz", ("dev-deps/android/media-runtime/downloads/ffmpeg-9.0.2.tar.xz",)),
    ("openssl-3.5.9.tar.gz", "603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a", "https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz", ("builds/work/android/app/_deps/pilon_openssl-subbuild/pilon_openssl-populate-prefix/src/openssl-3.5.9.tar.gz",)),
    ("libssh2-1.11.1.tar.gz", "d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7", "https://www.libssh2.org/download/libssh2-1.11.1.tar.gz", ("builds/work/android/app/_deps/libssh2-subbuild/libssh2-populate-prefix/src/libssh2-1.11.1.tar.gz",)),
)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def contains_private_key(path):
    with path.open("rb") as stream:
        tail = b""
        while chunk := stream.read(1024 * 1024):
            data = tail + chunk
            if PRIVATE_KEY.search(data):
                return True
            tail = data[-128:]
    return False


def source_files(root):
    manifest = root / SOURCE_MANIFEST
    if manifest.is_symlink() or not manifest.is_file():
        raise ValueError(f"Source release requires a regular {SOURCE_MANIFEST}")
    checked_directories = {root}
    yield manifest
    names = manifest.read_text(encoding="utf-8").splitlines()
    for name in sorted(set(names)):
        if not name:
            continue
        relative = Path(name)
        if (relative.is_absolute() or ".." in relative.parts or not relative.parts or
                relative.parts[0] not in SOURCE_PATHS or relative.as_posix() != name):
            raise ValueError(f"Invalid source release path: {name}")
        if relative.parts[:4] == ("addons", "calendar", "server", "data"):
            continue
        if any(part in SKIP_DIRECTORIES or part in {"build", "builds"}
               or part.startswith(("build-", "cmake-build-")) for part in relative.parts[:-1]):
            continue
        if (relative.name in PRIVATE_NAMES or ".backup" in relative.name or relative.suffix.lower() in PRIVATE_SUFFIXES
                or (relative.name.startswith(".env") and relative.name != ".env.example")):
            continue
        path = root / relative
        for parent in path.parents:
            if parent in checked_directories:
                break
            if parent.is_symlink():
                raise ValueError(f"Source archive cannot contain symbolic links: {parent.relative_to(root)}")
            checked_directories.add(parent)
        if path.is_symlink():
            raise ValueError(f"Source archive cannot contain symbolic links: {relative}")
        if not path.is_file():
            raise ValueError(f"Missing source release file: {relative}")
        if contains_private_key(path):
            raise ValueError(f"Private key material is forbidden in a release: {relative}")
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


def write_inventory(stage, kind, corresponding=False):
    script_roots = [stage / "src/scripts", *sorted((stage / "addons").glob("*/scripts"))]
    recipes = {str(path.relative_to(stage)): digest(path) for folder in script_roots for path in sorted(folder.rglob("*.sh"))}
    notices = {str(path.relative_to(stage)): digest(path) for path in sorted(stage.rglob("*"))
               if path.is_file() and (path.name == "DEPENDENCIES.json"
                   or any(word in path.name.upper() for word in ("LICENSE", "LICENCE", "COPYING", "NOTICE", "SOURCE.TXT")))}
    inventory = {"format": 1, "artifact": kind, "application_source": "Included in this archive", "build_recipes_sha256": recipes,
                 "third_party_notices_sha256": notices, "native_runtimes": [],
                 "corresponding_source_requested": corresponding,
                 "third_party_source_requirement": "Pinned source URLs, hashes, patches and build commands are included. When --corresponding-source is used, see the separate source report and its matching archive assets for validated cached sources and any remaining gaps. Review missing source or relink requirements before binary distribution."}
    (stage / "SOURCE-INVENTORY.json").write_text(json.dumps(inventory, indent=2, sort_keys=True) + "\n")
    if kind == "linux":
        (stage / "BUNDLE.txt").write_text("Run .bin/pilon from this extracted directory. Install the Linux dependencies in docs/install.md. This application bundle uses system Qt and shared libraries; it is not self-contained. Optional Python, inference and Office runtimes use the included src/scripts setup commands. Models and user data are not included. Install a launcher with python3 src/scripts/install-desktop-entry.py.\n")


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


def stage_linux(stage, binary, packages):
    if not os.access(binary, os.X_OK):
        raise ValueError("Linux release input must be executable")
    copy_file(binary, stage / ".bin/pilon")
    for addon in ADDONS:
        copy_file(packages / (addon + ".piext"), stage / ".bin/addons" / (addon + ".piext"))
    (stage / ".bin/addons/.install-packages").touch()


def verify_android(apk, apksigner):
    jdk = Path(os.environ.get("JAVA_HOME") or Path.home() / "Android/jdk-21")
    environment = {**os.environ, "JAVA_HOME": str(jdk), "PATH": str(jdk / "bin") + os.pathsep + os.environ.get("PATH", "")}
    result = subprocess.run([str(apksigner), "verify", "--print-certs", str(apk)], env=environment, capture_output=True, text=True, check=True)
    if "CN=Android Debug" in result.stdout or "CN=Android Debug" in result.stderr:
        raise ValueError("Android release packaging refuses the development signing certificate")


def stage_android_dependency_notices(apk, destination):
    prefix = "assets/licenses/android-dependencies/"
    with ZipFile(apk) as archive:
        if prefix + "DEPENDENCIES.json" not in archive.namelist():
            raise ValueError("Android APK is missing its dependency notice inventory")
        for entry in archive.infolist():
            if entry.is_dir() or not entry.filename.startswith(prefix):
                continue
            name = entry.filename[len(prefix):]
            relative = Path(name)
            if not relative.parts or relative.is_absolute() or ".." in relative.parts or relative.as_posix() != name:
                raise ValueError("Invalid APK dependency notice path: " + name)
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(entry) as source, target.open("xb") as output:
                shutil.copyfileobj(source, output)


def linux_build_metadata(root, binary, build_dir):
    def elf_metadata(path):
        data = subprocess.check_output(["readelf", "--wide", "--notes", "--dynamic", "--", str(path)], text=True,
                                       stderr=subprocess.PIPE, env={**os.environ, "LC_ALL": "C"})
        identity = re.search(r"\bBuild ID: ([0-9a-fA-F]+)\b", data)
        libraries = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", data)
        return identity.group(1).lower() if identity else None, libraries

    if binary is None or build_dir is None:
        raise ValueError("Linux corresponding sources require the packaged executable and its CMake build directory.")
    try:
        settings = dict(re.findall(r"^([^\s#/:=][^\n:=]*):[A-Z]+=(.*)$",
                                  (build_dir / "CMakeCache.txt").read_text(), re.MULTILINE))
        build_binary = build_dir / "pilon"
        identity, libraries = elf_metadata(binary)
        linked_identity, linked_libraries = elf_metadata(build_binary)
    except (OSError, subprocess.CalledProcessError) as error:
        raise ValueError("Cannot read the Linux binary or its CMake build metadata. Check readelf and --linux-build-dir.") from error
    if Path(settings.get("CMAKE_HOME_DIRECTORY", "")).resolve() != (root / "src").resolve():
        raise ValueError("The selected CMake build does not match this source checkout.")
    same_build = identity and identity == linked_identity
    if libraries != linked_libraries or (not same_build and digest(binary) != digest(build_binary)):
        raise ValueError("The packaged executable does not match the selected CMake build.")
    if not settings.get("FETCHCONTENT_BASE_DIR"):
        raise ValueError("The selected CMake build does not record its FetchContent cache.")
    return settings, identity, libraries


def corresponding_sources(root, output_dir, stem, kind, application_archive, *, linux_binary=None,
                          linux_build_dir=None, dev_dir=None):
    dev_dir = dev_dir or root
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

    if kind == "linux":
        report["external"] = []
        try:
            settings, identity, libraries = linux_build_metadata(root, linux_binary, linux_build_dir)
        except ValueError as error:
            report["missing"].append({"component": "Linux CMake build provenance", "reason": str(error)})
        else:
            report["linux_build_id"] = identity
            libical_sonames = sorted(name for name in libraries if re.fullmatch(r"libical(?:ss|vcal)?\.so(?:\..+)?", name))
            cache = linux_build_dir / settings["FETCHCONTENT_BASE_DIR"]
            for name, expected, url, _ in SOURCE_ARCHIVES:
                dependency = name.split("-", 1)[0]
                if dependency == "libical" and libical_sonames:
                    report["external"].append({"component": dependency, "sonames": libical_sonames,
                                               "version": settings.get("LIBICAL_VERSION") or None})
                    continue
                if dependency == "libical" and settings.get("LIBICAL_FOUND", "").upper() in {"1", "ON", "YES", "TRUE", "Y"}:
                    report["missing"].append({"component": dependency, "reason": "The build used a system static libical whose exact source is not recorded."})
                    continue
                if settings.get("FETCHCONTENT_SOURCE_DIR_" + dependency.upper()):
                    report["missing"].append({"component": dependency, "reason": "The build used a local FetchContent source override. A pinned download cannot establish its provenance."})
                    continue
                legacy = cache / f"{dependency}-subbuild" / f"{dependency}-populate-prefix" / "src" / name
                direct = cache / f"{dependency}-tmp" / name
                paths = [path for path in (legacy, direct) if path.is_file() and not path.is_symlink()]
                path = next((path for path in paths if digest(path) == expected), paths[0] if paths else legacy)
                add_archive(path, name, expected, url, validation="Pinned SHA-256 from the matching Linux build's FetchContent cache")

    else:
        for name, expected, url, candidates in SOURCE_ARCHIVES + (ANDROID_SOURCE_ARCHIVES if kind == "android" else ()):
            path = next((dev_dir / candidate for candidate in candidates if (dev_dir / candidate).is_file()), dev_dir / candidates[0])
            add_archive(path, name, expected, url, validation="Pinned application build-recipe SHA-256")

    if kind == "android":
        cache = dev_dir / "builds/work/publication/sources"
        cache.mkdir(parents=True, exist_ok=True)
        for name, checkout, commit, url in (
            ("qtbase", dev_dir / "dev-deps/android/qt-platform/upstream", "025bdad181de241e81bf853c8a2d7bf3d19261a9", "https://github.com/qt/qtbase"),
            ("nucleo", dev_dir / "dev-deps/android/codex/cargo/git/checkouts/nucleo-425d994cd74b3654/4253de9", "4253de9faabb4e5c6d81d946a5e35a90f87347ee", "https://github.com/helix-editor/nucleo"),
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

        codex_release = tomllib.loads((root / "addons/assistant/scripts/android/codex-release.toml").read_text())
        codex_tag = "rust-v" + codex_release["version"]
        codex_archive = "codex-" + codex_release["version"] + ".tar.gz"
        codex_cache = dev_dir / "dev-deps/android/codex"
        add_archive(codex_cache / "downloads" / codex_archive, codex_archive, codex_release["source_sha256"],
                    "https://codeload.github.com/openai/codex/tar.gz/refs/tags/" + codex_tag,
                    component="Codex", commit=codex_release["commit"], validation="Pinned Android build-recipe SHA-256")
        metadata = codex_cache / "dependencies.json"
        lock = codex_cache / "source" / ("codex-" + codex_tag) / "codex-rs/Cargo.lock"
        packages = json.loads(metadata.read_text())["packages"] if metadata.is_file() else []
        matching_metadata = any(item["name"] == "codex-cli" and item["version"] == codex_release["version"]
                                for item in packages)
        if matching_metadata and lock.is_file():
            locked = {(item["name"], item["version"]): item.get("checksum") for item in tomllib.loads(lock.read_text())["package"] if item.get("checksum")}
            for item in packages:
                if not re.search(r"(?:GPL|MPL|EPL|CDDL)", item.get("license") or "") or not (item.get("source") or "").startswith("registry+"):
                    continue
                source = Path(item["manifest_path"]).parent
                name = item["name"] + "-" + item["version"] + ".crate"
                archive = source.parents[2] / "cache" / source.parent.name / name
                expected = locked.get((item["name"], item["version"]))
                if expected:
                    add_archive(archive, name, expected, "https://crates.io/api/v1/crates/" + item["name"] + "/" + item["version"] + "/download", license=item["license"], validation="Frozen Cargo.lock checksum; conservative superset selected from dependency metadata")
        else:
            report["missing"].append({"component": "Codex copyleft dependency sources", "reason": "Cargo.lock or dependency metadata for the pinned Codex version is unavailable"})

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
                if contains_private_key(source):
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


def stage_package(args, root, output_dir):
    dev_dir = getattr(args, "dev_dir", root)
    version = project_version(root)
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
        if args.kind == "linux":
            packages = args.linux_binary.parent / "addons"
            if not all((packages / (addon + ".piext")).is_file() for addon in ADDONS):
                build_dir = getattr(args, "linux_build_dir", None) or dev_dir / "builds/work/linux"
                linux_build_metadata(root, args.linux_binary, build_dir)
                packages = build_dir / "addons"
            stage_linux(stage, args.linux_binary, packages)
        if args.kind == "android":
            licenses = dev_dir / "builds/android/licenses"
            if licenses.is_symlink():
                raise ValueError("Android notices cannot contain symbolic links")
            notices = []
            for component in ANDROID_NOTICE_COMPONENTS:
                folder = licenses / component
                if folder.is_symlink():
                    raise ValueError("Android notices cannot contain symbolic links")
                for path in sorted(folder.rglob("*")):
                    if path.is_symlink():
                        raise ValueError("Android notices cannot contain symbolic links")
                    if path.is_file():
                        notices.append(path)
            if not any("LICENSE" in path.name for path in notices):
                raise ValueError("Android release requires the staged third-party notices")
            for path in notices:
                copy_file(path, stage / "third-party-notices" / path.relative_to(licenses))
            stage_android_dependency_notices(args.android_apk, stage / "third-party-notices/android-dependencies")
            apk = output_dir / (stem + ".apk")
            copy_file(args.android_apk, apk.with_suffix(".apk.part"))
            apk.with_suffix(".apk.part").replace(apk)
            (stage / "APK-SHA256SUMS").write_text(digest(apk) + "  " + apk.name + "\n")
            outputs.append(apk)
        write_inventory(stage, args.kind, corresponding=getattr(args, "corresponding_source", False))
        archive = output_dir / (stage.name + ".tar.gz")
        write_archive(stage, archive, int(os.environ.get("SOURCE_DATE_EPOCH", "0")))
        outputs.append(archive)
        if getattr(args, "corresponding_source", False):
            source_options = {}
            if args.kind == "linux":
                linux_build_dir = getattr(args, "linux_build_dir", None)
                if linux_build_dir is None:
                    parent = args.linux_binary.resolve().parent
                    linux_build_dir = parent if (parent / "CMakeCache.txt").is_file() else dev_dir / "builds/work/linux"
                source_options = {"linux_binary": stage / ".bin/pilon", "linux_build_dir": linux_build_dir.resolve()}
            outputs.extend(corresponding_sources(root, output_dir, stem, args.kind, archive,
                                                dev_dir=dev_dir, **source_options))
    checksums = output_dir / (stem + "-SHA256SUMS")
    checksums.write_text("".join(digest(path) + "  " + path.name + "\n" for path in outputs))
    return outputs + [checksums]


def package(args, root=ROOT):
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    with (output_dir / ".pilon-release.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError(f"Another Pilon release is being staged in {output_dir}") from error
        with tempfile.TemporaryDirectory(prefix=".pilon-release-", dir=output_dir) as directory:
            staged = stage_package(args, root, Path(directory))
            destinations = [output_dir / source.name for source in staged]
            for destination in destinations:
                if destination.is_symlink() or (destination.exists() and not destination.is_file()):
                    raise ValueError(f"Expected a regular release output: {destination}")
            # Publish the checksum last so it only describes a fully staged set.
            for source, destination in zip(staged, destinations, strict=True):
                source.replace(destination)
            return destinations


def main():
    load_environment()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=("source", "linux", "android"))
    parser.add_argument("--output-dir", type=Path, default=development_root() / "builds/releases")
    parser.add_argument("--linux-binary", type=Path, default=installation_root() / ".bin/pilon")
    parser.set_defaults(dev_dir=development_root())
    parser.add_argument("--linux-build-dir", type=Path, help="Matching CMake build directory for Linux corresponding sources")
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
