#!/usr/bin/env python3
"""Compile a Pilon source add-on into a Qt binary-resource .piext package."""
import argparse
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from urllib.parse import urlsplit

BUILTINS = {"files", "ssh", "assistant", "notes", "calendar", "devices"}
PACKAGE_ID = re.compile(r"[a-z][a-z0-9-]*(?:\.[a-z][a-z0-9-]*)+")
SECTION_ID = re.compile(r"[a-z][a-z0-9-]{0,63}")


def version(value):
    if not isinstance(value, str) or not re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", value):
        raise ValueError("Dependency versions must be numeric X.Y.Z without leading zeroes.")
    parts = tuple(int(part) for part in value.split("."))
    if any(part > 2147483647 for part in parts):
        raise ValueError("Dependency version components are too large.")
    return parts


def validate(folder, builtin):
    if folder.is_symlink() or not folder.is_dir():
        raise ValueError("Choose a source folder, not a symbolic link.")
    paths = sorted(folder.rglob("*"))
    if any(path.is_symlink() for path in paths):
        raise ValueError("Packages cannot contain symbolic links.")
    manifest = folder / "manifest.json"
    if not manifest.is_file() or manifest.stat().st_size > 65536:
        raise ValueError("The folder needs a manifest.json of at most 64 KiB.")
    data = json.loads(manifest.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError("manifest.json must be an object.")
    allowed = {"id", "name", "description", "version", "apiVersion", "page", "settings", "runtime", "startOnEnable", "icon", "dependencies", "extensionPoints", "integrations", "lifecycle", "widgets", "source", "portableSettings"}
    if data.keys() - allowed:
        raise ValueError("Unknown manifest fields: " + ", ".join(sorted(data.keys() - allowed)))
    if type(data.get("apiVersion")) is not int or data["apiVersion"] != 1:
        raise ValueError("apiVersion must be 1.")
    if "source" in data:
        source = urlsplit(data["source"]) if isinstance(data["source"], str) else None
        if not source or source.scheme != "https" or not source.hostname or source.username or source.password or source.query or source.fragment:
            raise ValueError("The add-on source must be a public HTTPS URL.")
    if "portableSettings" in data:
        keys = data["portableSettings"]
        if (not isinstance(keys, list) or len(keys) > 256
                or any(not isinstance(key, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}", key) for key in keys)
                or len(set(keys)) != len(keys)):
            raise ValueError("Portable settings must have unique setting names.")
    addon_id = data.get("id")
    def valid_id(value):
        return isinstance(value, str) and len(value) <= 128 and (value in BUILTINS or PACKAGE_ID.fullmatch(value))

    if not valid_id(addon_id) or (builtin and addon_id not in BUILTINS):
        raise ValueError("Use a namespaced add-on ID or a known shipped ID.")

    def text(value, limit=128):
        return isinstance(value, str) and bool(value.strip()) and len(value) <= limit and not re.search(r"[\x00-\x1f\x7f]", value)

    def entry(value, qml=True):
        if not isinstance(value, str) or not value or any(part in ("", ".", "..") for part in value.split("/")):
            raise ValueError("Entries must be relative paths inside the package.")
        if "\\" in value or ":" in value or re.search(r"[\x00-\x1f\x7f]", value):
            raise ValueError("Entries must be local package paths.")
        path = folder / value
        if not path.is_file() or not path.resolve().is_relative_to(folder.resolve()) or (qml and not value.endswith(".qml")):
            raise ValueError("Entry not found inside the package: " + value)

    if not text(data.get("name")) or not text(data.get("version"), 64):
        raise ValueError("The manifest needs a name and version.")
    if "description" in data and not text(data["description"], 200):
        raise ValueError("The description must be nonblank text of at most 200 characters without control characters.")
    for field in ("page", "runtime", "lifecycle"):
        if field in data:
            entry(data[field])
    if "icon" in data:
        entry(data["icon"], False)
    if "startOnEnable" in data and type(data["startOnEnable"]) is not bool:
        raise ValueError("startOnEnable must be a boolean.")
    if data.get("startOnEnable") and "runtime" not in data:
        raise ValueError("startOnEnable requires a runtime.")
    widgets = data.get("widgets", [])
    if not isinstance(widgets, list) or len(widgets) > 16:
        raise ValueError("widgets must be an array of at most 16 entries.")
    if widgets and "runtime" not in data:
        raise ValueError("Widget declarations require a runtime.")
    seen = set()
    for widget in widgets:
        if not isinstance(widget, dict) or widget.keys() != {"id", "name"}:
            raise ValueError("Each widget needs exactly id and name.")
        widget_id = widget["id"]
        if not isinstance(widget_id, str) or not SECTION_ID.fullmatch(widget_id) or widget_id in seen or not text(widget["name"]):
            raise ValueError("Widgets need unique IDs and nonblank names of at most 128 characters without control characters.")
        seen.add(widget_id)
    sections = data.get("settings", [])
    if not isinstance(sections, list) or len(sections) > 32:
        raise ValueError("settings must be an array of at most 32 sections.")
    seen = set()
    for section in sections:
        if not isinstance(section, dict) or section.keys() != {"id", "name", "entry"}:
            raise ValueError("Each settings section needs exactly id, name and entry.")
        section_id = section.get("id")
        if not isinstance(section_id, str) or not SECTION_ID.fullmatch(section_id) or section_id in seen or not text(section.get("name")):
            raise ValueError("Settings sections need unique IDs and names.")
        seen.add(section_id)
        entry(section["entry"])
    def records(field):
        values = data.get(field, [])
        if not isinstance(values, list) or len(values) > 32:
            raise ValueError(field + " must be an array of at most 32 entries.")
        return values

    seen = set()
    for dependency in records("dependencies"):
        if not isinstance(dependency, dict) or not {"id", "minVersion"} <= dependency.keys() or dependency.keys() - {"id", "minVersion", "maxVersion"}:
            raise ValueError("Dependencies need id, minVersion and optional maxVersion.")
        dependency_id = dependency["id"]
        if not valid_id(dependency_id) or dependency_id == addon_id or dependency_id in seen:
            raise ValueError("Dependencies need unique IDs other than this add-on.")
        seen.add(dependency_id)
        minimum = version(dependency["minVersion"])
        if "maxVersion" in dependency and version(dependency["maxVersion"]) <= minimum:
            raise ValueError("maxVersion must be greater than minVersion.")
    seen = set()
    for point in records("extensionPoints"):
        if not isinstance(point, dict) or point.keys() != {"id", "apiVersion"} or not isinstance(point["id"], str) or not SECTION_ID.fullmatch(point["id"]) or point["id"] in seen or type(point["apiVersion"]) is not int or not 1 <= point["apiVersion"] <= 2147483647:
            raise ValueError("Extension points need unique IDs and a positive integer apiVersion.")
        seen.add(point["id"])
    seen = set()
    for integration in records("integrations"):
        fields = {"id", "name", "target", "minVersion", "point", "apiVersion", "entry"}
        if not isinstance(integration, dict) or not fields <= integration.keys() or integration.keys() - fields - {"enabledByDefault"}:
            raise ValueError("Integrations need id, name, target, minVersion, point, apiVersion and entry.")
        if "enabledByDefault" in integration and type(integration["enabledByDefault"]) is not bool:
            raise ValueError("Integration enabledByDefault must be a boolean.")
        integration_id = integration["id"]
        if not isinstance(integration_id, str) or not SECTION_ID.fullmatch(integration_id) or integration_id in seen or not text(integration["name"]) or not valid_id(integration["target"]) or integration["target"] == addon_id:
            raise ValueError("Integrations need unique IDs, names and another target add-on.")
        if not isinstance(integration["point"], str) or not SECTION_ID.fullmatch(integration["point"]) or type(integration["apiVersion"]) is not int or not 1 <= integration["apiVersion"] <= 2147483647:
            raise ValueError("Integrations need a point ID and a positive integer apiVersion.")
        seen.add(integration_id)
        version(integration["minVersion"])
        entry(integration["entry"])
    if not any(data.get(field) for field in ("page", "runtime", "settings", "integrations", "lifecycle")):
        raise ValueError("The add-on needs a page, runtime, settings, integration or lifecycle entry.")
    for path in paths:
        if not path.is_file() and not path.is_dir():
            raise ValueError("Packages may contain only ordinary files and folders.")
    return [path for path in paths if path.is_file()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("--output", "-o", required=True, type=pathlib.Path)
    parser.add_argument("--rcc", help="Path to Qt 6 rcc")
    parser.add_argument("--builtin", action="store_true", help="Build a known shipped Pilon package")
    args = parser.parse_args()
    try:
        source = args.source.absolute()
        output = args.output.absolute()
        if output.suffix != ".piext" or output.is_symlink() or output.resolve().is_relative_to(source.resolve()):
            raise ValueError("Output must be a .piext file outside the source folder, not a symbolic link.")
        files = validate(source, args.builtin)
        candidates = [args.rcc] if args.rcc else [shutil.which("rcc6"), "/usr/lib/qt6/rcc", "/usr/lib/qt6/libexec/rcc", shutil.which("rcc")]
        rcc = None
        for candidate in candidates:
            if not candidate or not pathlib.Path(candidate).is_file():
                continue
            try:
                version = subprocess.run([candidate, "--version"], text=True, capture_output=True, check=True)
            except (OSError, subprocess.CalledProcessError):
                if args.rcc:
                    raise
                continue
            if re.search(r"\b6\.", version.stdout + version.stderr):
                rcc = candidate
                break
        if rcc is None:
            raise ValueError("Qt 6 rcc is required; provide its path with --rcc.")
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".piext-build-", dir=output.parent) as temporary:
            temporary = pathlib.Path(temporary)
            collection = ET.Element("RCC")
            resource = ET.SubElement(collection, "qresource", prefix="/")
            for path in files:
                element = ET.SubElement(resource, "file", alias=path.relative_to(source).as_posix())
                element.text = str(path)
            qrc = temporary / "package.qrc"
            ET.ElementTree(collection).write(qrc, encoding="utf-8", xml_declaration=True)
            compiled = temporary / "package.piext"
            # Android Qt supports zlib resources without the desktop-only zstd feature.
            subprocess.run([rcc, "--binary", "--format-version", "3", "--compress-algo", "zlib",
                            "--output", str(compiled), str(qrc)], check=True)
            compiled.chmod(0o600)
            compiled.replace(output)
        print(output)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Cannot compile add-on: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    from environment import load_environment
    load_environment()
    sys.exit(main())
