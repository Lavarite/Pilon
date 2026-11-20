#!/usr/bin/env bash
set -euo pipefail

lint=.github/scripts/commit_lint.py
python3 .github/scripts/commit_lint_tests.py

if [[ "$GITHUB_EVENT_NAME" == pull_request ]]; then
    python3 "$lint" history "$PR_HEAD" "$PR_BASE"
elif [[ "$GITHUB_REF" == refs/tags/* ]]; then
    python3 "$lint" tag "$GITHUB_SHA"
elif [[ "$BEFORE" != 0000000000000000000000000000000000000000 ]]; then
    # On a force push, the previous tip can be outside the checked-out history.
    git cat-file -e "$BEFORE^{commit}" 2>/dev/null || git fetch --no-tags origin "$BEFORE"
    python3 "$lint" history "$GITHUB_SHA" "$BEFORE"
elif [[ "$GITHUB_REF" == "refs/heads/$DEFAULT_BRANCH" ]]; then
    python3 "$lint" history "$GITHUB_SHA"
else
    python3 "$lint" history "$GITHUB_SHA" "refs/remotes/origin/$DEFAULT_BRANCH"
fi
