#!/usr/bin/env bash
# corral_script_spawner.sh TAG (F3): starts a background job, then exits.
"$(dirname "$0")/corral_sleeper" "$1" 300 &
exit 0
