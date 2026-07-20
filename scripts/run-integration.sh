#!/usr/bin/env bash
# Runs the integration suite in two parts. The checks for a missing delegation
# run first, in the caller's own cgroup. Both corpora then run inside one
# delegated systemd scope, which enforced mode needs. Without a systemd user
# session, the corpora run without a scope and the enforced tests are skipped.
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
SUITE="$REPO/tests/integration/run_corpus.py"
PYTHON="${PYTHON:-python3}"
status=0

"$PYTHON" "$SUITE" OutsideScope || status=1

if systemd-run --user --scope -p Delegate=yes --quiet true 2>/dev/null; then
  systemd-run --user --scope -p Delegate=yes --quiet \
    "$PYTHON" "$SUITE" FallbackCorpus EnforcedCorpus || status=1
else
  echo "run-integration.sh: no systemd user session, so enforced mode is not tested" >&2
  "$PYTHON" "$SUITE" FallbackCorpus EnforcedCorpus || status=1
fi
exit "$status"
