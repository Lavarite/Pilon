#!/usr/bin/env python3
"""Scan application QML without pulling the repository's test imports into its APK."""
import os
from pathlib import Path
import sys


def application_files(root):
    root = Path(root)
    roots = [root]
    # Built-in QML moved beside its add-on; server and test code are not app imports.
    if root.name == "src" and (root / "CMakeLists.txt").is_file():
        for addon in sorted((root.parent / "addons").glob("*")):
            roots.extend(addon / name for name in ("ui", "package") if (addon / name).is_dir())
    for source in roots:
        for directory, children, files in os.walk(source):
            children[:] = [name for name in children if name != "tests" and not name.startswith(".")]
            for name in files:
                if Path(name).suffix in (".qml", ".js"):
                    yield str(Path(directory, name).resolve())


def main():
    scanner, *arguments = sys.argv[1:]
    forwarded, files = [], set()
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        index += 1
        if argument != "-rootPath":
            forwarded.append(argument)
            continue
        while index < len(arguments) and not arguments[index].startswith("-"):
            files.update(application_files(arguments[index]))
            index += 1
    if files:
        forwarded.extend(["-qmlFiles", *sorted(files)])
    os.execv(scanner, [scanner, *forwarded])


if __name__ == "__main__":
    from environment import load_environment
    load_environment()
    main()
