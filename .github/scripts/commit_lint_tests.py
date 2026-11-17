import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPTS = Path(__file__).resolve().parent
LINT = SCRIPTS / "commit_lint.py"
HOOKS = SCRIPTS.parent / "hooks"


class CommitLintTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        self.project = root / "project"
        self.project.mkdir()
        self.env = {**os.environ, "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"}
        self.git("init", "--initial-branch=main")
        self.git("config", "user.name", "Pilon lint fixture")
        self.git("config", "user.email", "pilon@example.test")
        self.git("config", "core.hooksPath", str(HOOKS))
        self.git("init", "--bare", str(root / "remote.git"))
        self.git("remote", "add", "origin", str(root / "remote.git"))

    def run_command(self, *args, success=True):
        result = subprocess.run(
            args, cwd=self.project, env=self.env, capture_output=True, text=True
        )
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result.stdout.strip()

    def git(self, *args, success=True):
        return self.run_command("git", *args, success=success)

    def commit(self, title, success=True, skip_hook=False):
        arguments = ["--no-verify"] if skip_hook else []
        self.git("commit", "--allow-empty", *arguments, "-m", title, success=success)

    def test_commit_requires_scope_and_allows_breaking_change(self):
        self.commit("fix: no scope", success=False)
        self.commit("fix(Notes): uppercase", success=False)
        self.commit("fix(notes-ui)!: allow Unicode café")

    def test_existing_push_checks_all_new_commits(self):
        self.commit("chore(release): 1.0.0")
        self.git("push", "origin", "main")
        base = self.git("rev-parse", "HEAD")
        self.commit("missing scope in the middle", skip_hook=True)
        self.commit("test(ci): valid tip")
        self.run_command(sys.executable, str(LINT), "history", "HEAD", base, success=False)
        self.git("push", "origin", "main", success=False)

    def test_new_branch_checks_history_missing_from_remote(self):
        self.commit("old message already on remote", skip_hook=True)
        self.git("-c", "core.hooksPath=" + os.devnull, "push", "origin", "main")
        self.git("checkout", "-b", "feature")
        self.commit("docs(install): clarify the Linux command")
        self.git("push", "origin", "feature")
        self.git("checkout", "-b", "bad")
        self.commit("docs: no scope", skip_hook=True)
        self.git("push", "origin", "bad", success=False)

    def test_tag_lints_its_commit_and_ref_deletion_is_allowed(self):
        self.commit("chore(release): 1.0.0")
        self.git("tag", "-a", "v1.0.0", "-m", "Pilon 1.0.0")
        self.git("push", "origin", "v1.0.0")
        self.git("push", "origin", "--delete", "v1.0.0")
        self.commit("missing scope", skip_hook=True)
        self.git("tag", "v1.1.0")
        self.git("push", "origin", "v1.1.0", success=False)


if __name__ == "__main__":
    unittest.main()
