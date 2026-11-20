import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCOPE = Path(__file__).with_name("ci_scope.py")


class CiScopeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "project"
        self.root.mkdir()
        self.output = self.root.parent / "output"
        self.env = {**os.environ, "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"}
        self.git("init", "-q", "--initial-branch=main")
        self.git("config", "user.name", "CI fixture")
        self.git("config", "user.email", "ci@example.test")
        self.write("src/file.qml", "old")
        self.write("docs/guide.md", "old")
        self.base = self.commit()
        self.git("update-ref", "refs/remotes/origin/main", self.base)

    def git(self, *args):
        return subprocess.check_output(["git", *args], cwd=self.root, env=self.env, text=True).strip()

    def write(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    def commit(self):
        self.git("add", "-A")
        self.git("commit", "-qm", "test(ci): add a fixture")
        return self.git("rev-parse", "HEAD")

    def scope(self, ref, head, base="", event="push", expected_status=0):
        env = {**self.env, "GITHUB_EVENT_NAME": event, "GITHUB_REF": ref, "GITHUB_SHA": head,
               "BEFORE": base if event == "push" else "", "PR_BASE": base if event == "pull_request" else "",
               "DEFAULT_BRANCH": "main", "GITHUB_OUTPUT": str(self.output)}
        self.output.write_text("")
        result = subprocess.run([sys.executable, str(SCOPE)], cwd=self.root, env=env,
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, expected_status, result.stderr)
        return self.output.read_text().strip()

    def test_docs_only_pr_merge_commit_and_main_push_use_lightweight_checks(self):
        self.git("switch", "-qc", "topic")
        self.write("docs/guide.md", "docs only")
        self.write("docs/screenshots/new.png", "image fixture")
        topic = self.commit()
        self.git("switch", "-q", "main")
        self.write("src/file.qml", "already on main")
        updated_base = self.commit()
        self.git("merge", "-q", "--no-ff", topic, "-m", "Fixture PR merge")
        merge_head = self.git("rev-parse", "HEAD")
        self.assertEqual(self.scope("refs/pull/1/merge", merge_head, updated_base, "pull_request"), "scope=style")
        self.assertEqual(self.scope("refs/heads/main", merge_head, updated_base), "scope=style")

    def test_build_changes_and_renames_out_of_source_require_full_checks(self):
        self.git("mv", "src/file.qml", "docs/moved.md")
        self.write("README.md", "docs change alongside the source deletion")
        head = self.commit()
        self.assertEqual(self.scope("refs/heads/main", head, self.base), "scope=full")

    def test_unknown_diff_or_non_document_in_docs_requires_full_checks(self):
        self.write("docs/run.sh", "echo fixture")
        head = self.commit()
        self.assertEqual(self.scope("refs/heads/main", head, self.base), "scope=full")
        self.assertEqual(self.scope("refs/heads/main", head, "0" * 40), "scope=full")
        self.assertEqual(self.scope("refs/heads/main", head, "f" * 40), "scope=full")

    def test_release_requires_version_tag_on_main(self):
        self.assertEqual(self.scope("refs/tags/v1.2.3", self.base), "scope=release")
        self.assertEqual(self.scope("refs/tags/snapshot", self.base), "scope=style")
        self.write("src/file.qml", "not yet on main")
        head = self.commit()
        self.assertEqual(self.scope("refs/tags/v1.2.4", head, expected_status=1), "")


if __name__ == "__main__":
    unittest.main()
