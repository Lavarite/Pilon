import os
import re
import subprocess
import sys


def is_documentation(path):
    return path in (b"README.md", b"CONTRIBUTING.md") or (
        path.startswith(b"docs/") and path.endswith(b".md")
    ) or (
        path.startswith(b"docs/screenshots/") and path.endswith(b".png")
    )


def scope(env):
    ref = env["GITHUB_REF"]
    head = env["GITHUB_SHA"]
    if ref.startswith("refs/tags/"):
        if not re.fullmatch(r"refs/tags/v[0-9]+\.[0-9]+\.[0-9]+", ref):
            return "style"
        origin = "refs/remotes/origin/" + env["DEFAULT_BRANCH"]
        if subprocess.run(["git", "merge-base", "--is-ancestor", head, origin]).returncode != 0:
            raise ValueError("Version tags must point to a commit on the default branch.")
        return "release"

    base = env.get("PR_BASE") if env["GITHUB_EVENT_NAME"] == "pull_request" else env.get("BEFORE")
    if not base or not base.strip("0"):
        return "full"
    # Compare the checked-out merge tree on PRs. Include both sides of a rename.
    result = subprocess.run(["git", "diff", "--no-renames", "--name-only", "-z", base, head, "--"],
                            stdout=subprocess.PIPE)
    if result.returncode != 0:
        print("The changed files could not be determined. Running the full checks.", file=sys.stderr)
        return "full"
    paths = result.stdout.split(b"\0")[:-1]
    return "style" if paths and all(is_documentation(path) for path in paths) else "full"


if __name__ == "__main__":
    try:
        selected = scope(os.environ)
    except ValueError as error:
        sys.exit(str(error))
    print(f"CI scope: {selected}")
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
        output.write(f"scope={selected}\n")
