#!/usr/bin/env bash
# corral_script_spawner.sh TAG (F3): starts a background job, then exits.
# The path is expanded before the '&': inside the job, $(...) would fork a
# second, short-lived process and make the leftover count vary.
dir=$(dirname "$0")
"$dir/corral_sleeper" "$1" 300 &
exit 0
