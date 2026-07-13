#!/usr/bin/env bash
# Runs fault F5 (a double-fork daemon whose launcher stays) under the naive
# runner, GNU timeout, and corral fallback, and shows what each leaves alive.
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${CORRAL_BUILD_DIR:-$REPO/build}"
F5="$BUILD/faults/corral_daemonizer_stay"

# demo NAME TAG COMMAND...: runs COMMAND, then lists the processes with TAG.
demo() {
  local name=$1 tag=$2
  shift 2
  echo "== $name"
  echo "   \$ $*"
  local start end
  start=$(date +%s%N)
  "$@" </dev/null
  local code=$?
  end=$(date +%s%N)
  echo "   exit $code after $(( (end - start) / 1000000 )) ms"
  sleep 0.2
  echo "   still alive:"
  local alive
  alive=$(pgrep -af "$tag")
  if [ -n "$alive" ]; then echo "$alive" | sed 's/^/     /'; else echo "     (nothing)"; fi
  pkill -KILL -f "$tag"
  echo
}

# NV waits for pipe EOF, which the daemon never sends, so it is stopped after
# 7 s, as bench/compare.py does.
demo "naive runner (NV)" "corraltag-demo-nv-$$" \
  timeout -s KILL 7 python3 "$REPO/bench/naive_runner.py" "$F5" "corraltag-demo-nv-$$"
demo "GNU timeout (TO)" "corraltag-demo-to-$$" \
  timeout -k 1s 2s "$F5" "corraltag-demo-to-$$"
demo "corral fallback (CF)" "corraltag-demo-cf-$$" \
  "$BUILD/corral" --cgroup-mode=fallback --wall 2s --grace 1s -- "$F5" "corraltag-demo-cf-$$"
