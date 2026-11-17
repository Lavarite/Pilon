#!/usr/bin/env python3
"""Require a scoped Conventional Commit subject for every new commit."""
import argparse
from pathlib import Path
import re
import subprocess
import sys


SUBJECT = re.compile(
    r"(build|chore|ci|docs|feat|fix|perf|refactor|revert|style|test)"
    r"\([a-z0-9]+(?:-[a-z0-9]+)*\)!?: \S.*"
)
EXPECTED = "type(scope): summary (for example, fix(notes): keep note actions visible)"


def git(*arguments):
    return subprocess.run(
        ["git", *arguments], check=True, stdout=subprocess.PIPE, encoding="utf-8", errors="replace"
    ).stdout


def check(message, label):
    subject = next((line for line in message.splitlines() if line.strip()), "")
    if SUBJECT.fullmatch(subject):
        return True
    print(f"{label}: {subject!r}\nExpected {EXPECTED}.", file=sys.stderr)
    return False


def check_history(*revisions):
    records = git("log", "-z", "--format=%h%x00%B", *revisions, "--").split("\0")
    valid = True
    for commit, message in zip(records[::2], records[1::2]):
        valid = check(message, commit) and valid
    return valid


def check_push(remote, updates):
    valid = True
    for update in updates:
        _, local_oid, remote_ref, remote_oid = update.split()
        if not local_oid.strip("0"):
            continue
        if remote_ref.startswith("refs/tags/"):
            valid = check_history("-1", local_oid + "^{commit}") and valid
        elif remote_ref.startswith("refs/heads/"):
            excluded = [remote_oid] if remote_oid.strip("0") else [f"--remotes={remote}"]
            valid = check_history(local_oid, "--not", *excluded) and valid
    return valid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="mode", required=True)
    commands.add_parser("message").add_argument("path", type=Path)
    history = commands.add_parser("history")
    history.add_argument("head")
    history.add_argument("base", nargs="?")
    commands.add_parser("tag").add_argument("ref")
    commands.add_parser("push").add_argument("remote")
    args = parser.parse_args()
    try:
        if args.mode == "message":
            return not check(args.path.read_text(encoding="utf-8", errors="replace"), str(args.path))
        if args.mode == "push":
            return not check_push(args.remote, sys.stdin)
        if args.mode == "tag":
            return not check_history("-1", args.ref + "^{commit}")
        excluded = ["--not", args.base] if args.base else []
        return not check_history(args.head, *excluded)
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"Unable to check commit subjects: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
