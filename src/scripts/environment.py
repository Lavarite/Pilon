#!/usr/bin/env python3
"""Load optional local build settings without evaluating shell code."""
import os
from pathlib import Path
import re
import shlex
import stat
import sys


ROOT = Path(__file__).resolve().parents[2]


def load_environment(root=ROOT):
    inherited = set(os.environ)
    def read_file(path):
        if not path.exists():
            return
        for number, line in enumerate(path.read_text().splitlines(), 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("export "):
                line = line[7:].lstrip()
            name, separator, raw = line.partition("=")
            name = name.strip()
            if not separator or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
                raise ValueError(f"Invalid environment assignment at {path}:{number}")
            values = shlex.split(raw, comments=True)
            if len(values) > 1:
                raise ValueError(f"Quote values containing spaces at {path}:{number}")
            if name not in inherited:
                os.environ[name] = values[0] if values else ""
    read_file(root / ".env")
    private = os.environ.get("PILON_RELEASE_ENV")
    if private:
        path = Path(private).expanduser().resolve(strict=True)
        if path.is_relative_to(root.resolve()):
            raise ValueError("PILON_RELEASE_ENV must be outside the repository")
        if path.stat().st_uid != os.getuid() or stat.S_IMODE(path.stat().st_mode) & 0o077:
            raise ValueError("PILON_RELEASE_ENV must be owned by this user with mode 0600 or stricter")
        read_file(path)
    os.environ["PILON_ENV_LOADED"] = "1"


if __name__ == "__main__":
    try:
        load_environment()
        if len(sys.argv) < 2:
            raise ValueError("Pass the build command to execute")
        os.execvp(sys.argv[1], sys.argv[1:])
    except (OSError, ValueError) as error:
        sys.exit(str(error))
