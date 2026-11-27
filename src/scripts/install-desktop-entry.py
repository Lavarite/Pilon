#!/usr/bin/env python3
"""Install Pilon's launcher, file types and icons for the current user."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


FILE_TYPES = ("note", "calendar", "extension")


def write_asset(target, data):
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as file:
        temporary = Path(file.name)
        file.write(data)
    try:
        temporary.chmod(0o644)
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)


def update_database(command, *arguments):
    tool = shutil.which(command)
    if tool:
        subprocess.run([tool, *map(str, arguments)], check=True)


def install(executable):
    executable = Path(executable).resolve(strict=True)
    if not os.access(executable, os.X_OK):
        raise ValueError(f"Not executable: {executable}")
    if any(char in str(executable) for char in "\n\r=%"):
        raise ValueError("The desktop entry cannot use this executable path")
    # Desktop Entry Specification: string escaping precedes Exec unquoting.
    # https://specifications.freedesktop.org/desktop-entry/latest/exec-variables.html
    quoted = ''.join('\\' + char if char in '\\"`$' else char for char in str(executable))
    quoted = quoted.replace('\\', '\\\\')
    source = Path(__file__).resolve().parents[2]
    data_home = Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local/share')
    assets = {Path("mime/packages/pilon.xml"): (source / "src/platform/linux/desktop/pilon-mime.xml").read_bytes()}
    app_icon = Path("icons/hicolor/scalable/apps/pilon.svg")
    if not (data_home / app_icon).exists():
        assets[app_icon] = (source / "src/assets/icons/pilon.svg").read_bytes()
    for kind in FILE_TYPES:
        name = f"application-x-pilon-{kind}.svg"
        directory = {"note": "addons/notes/assets/icons", "calendar": "addons/calendar/assets/icons", "extension": "src/assets/icons"}[kind]
        assets[Path("icons/hicolor/scalable/mimetypes") / name] = (source / directory / name).read_bytes()
    assets[Path("icons/hicolor/scalable/mimetypes/application-x-pilon-config.svg")] = (
        source / "src/assets/icons/cog-6-tooth.svg").read_bytes()
    mime_types = ''.join(f"application/x-pilon-{kind};" for kind in FILE_TYPES)
    text = ('[Desktop Entry]\nType=Application\nName=Pilon\n'
            f'Exec="{quoted}" %F\nIcon=pilon\nTerminal=false\n'
            f'Categories=Office;\nStartupWMClass=pilon\nMimeType={mime_types}\n')
    folder = data_home / 'applications'
    folder.mkdir(parents=True, exist_ok=True)
    target = folder / 'pilon.desktop'
    with tempfile.NamedTemporaryFile(mode='w', dir=folder, suffix='.desktop', delete=False) as file:
        temporary = Path(file.name)
        file.write(text)
    try:
        validator = shutil.which('desktop-file-validate')
        if validator:
            subprocess.run([validator, str(temporary)], check=True)
        for relative, data in assets.items():
            write_asset(data_home / relative, data)
        temporary.chmod(0o644)
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)
    update_database('update-mime-database', data_home / 'mime')
    update_database('gtk-update-icon-cache', '-f', '-t', data_home / 'icons/hicolor')
    update_database('update-desktop-database', folder)
    return target


if __name__ == '__main__':
    from environment import installation_root, load_environment
    load_environment()
    parser = argparse.ArgumentParser(description=__doc__)
    portable = Path(__file__).resolve().parents[2]
    candidates = [portable / '.bin/pilon', portable / 'bin/pilon',
                  installation_root() / '.bin/pilon', installation_root() / 'bin/pilon']
    default = next((candidate for candidate in candidates if candidate.is_file()), candidates[2])
    parser.add_argument('--executable', type=Path, default=default)
    args = parser.parse_args()
    print(f'Installed launcher: {install(args.executable)}')
