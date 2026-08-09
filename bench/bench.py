#!/usr/bin/env python3
"""Performance of corral: termination latency and per-run overhead.

Latency: F5, F6, and F10 under each corral mode; P50 and P95 of
verified_ms - trigger_ms from the audit record. Overhead: /bin/true run
directly, under corral fallback, and under corral enforced; P50 and P95 of the
wall time per run. Writes bench.json and bench.md.

CE needs a delegated cgroup, so run it inside one systemd scope:
    systemd-run --user --scope -p Delegate=yes --quiet python3 bench/bench.py
"""

import argparse
import json
import math
import os
import subprocess
import sys
import tempfile
import time

from compare import REPO, STANDARD, enforced_parent, machine  # also sets up `harness`
import harness

LATENCY_SCENARIOS = [
    ("F5", "corral_daemonizer_stay"),
    ("F6", "corral_term_ignorer"),
    ("F10", "corral_graceful"),
]


def percentile(values, p):
    """Nearest-rank percentile: the smallest value with p% of values at or below it."""
    ordered = sorted(values)
    return ordered[max(0, math.ceil(p / 100 * len(ordered)) - 1)]


def summary(values):
    return {"p50": percentile(values, 50), "p95": percentile(values, 95), "n": len(values)}


def mode_options(mode, ce_parent):
    if mode == "CE":
        return ["--cgroup-mode=enforced", "--cgroup-parent", ce_parent]
    return ["--cgroup-mode=fallback"]


def latency(build, mode, ce_parent, program, runs):
    """verified_ms - trigger_ms for `runs` runs of one fault program."""
    corral = os.path.join(build, "corral")
    values = []
    for _ in range(runs):
        tag = harness.new_tag()
        with tempfile.TemporaryDirectory() as tmp:
            record_path = os.path.join(tmp, "record.json")
            argv = [corral, "--quiet", "--json", record_path, *mode_options(mode, ce_parent),
                    *STANDARD, "--", os.path.join(build, "faults", program), tag]
            try:
                r = harness.run(argv, tag)
            finally:
                leaked = harness.kill_tagged(tag)
            if r.hung or r.survivors or leaked:
                sys.exit(f"bench.py: {program} under {mode} left survivors or hung")
            with open(record_path) as f:
                t = json.load(f)["termination"]
        values.append(t["verified_ms"] - t["trigger_ms"])
    return values


def overhead(argv, runs):
    """Wall time in milliseconds for `runs` runs of argv."""
    values = []
    for _ in range(runs):
        start = time.perf_counter()
        subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, check=True)
        values.append((time.perf_counter() - start) * 1000)
    return values


def write_markdown(path, results, info):
    lines = [
        "# corral performance",
        "",
        f"Machine: {info['model']}, {info['cpus']} CPUs, {info['distribution']}, "
        f"kernel {info['kernel']}.",
        "",
        "## Termination latency",
        "",
        "`verified_ms - trigger_ms` from the audit record: the time from the trigger until "
        "nothing is left. F6 ignores SIGTERM, so its latency includes the 1 s grace period.",
        "",
        "| Scenario | Mode | P50 (ms) | P95 (ms) | Runs |",
        "|---|---|---|---|---|",
    ]
    for row in results["latency"]:
        lines.append(f"| {row['scenario']} | {row['mode']} | {row['p50']} | {row['p95']} | "
                     f"{row['n']} |")
    lines += [
        "",
        "## Overhead",
        "",
        "Wall time per run of `/bin/true`, including process creation, measured from Python.",
        "",
        "| Runner | P50 (ms) | P95 (ms) | Runs |",
        "|---|---|---|---|",
    ]
    for row in results["overhead"]:
        lines.append(f"| {row['runner']} | {row['p50']:.2f} | {row['p95']:.2f} | {row['n']} |")
    if results["ce_unavailable"]:
        lines += ["", f"CE not measured: {results['ce_unavailable']}"]
    lines.append("")
    with open(path, "w") as f:
        f.write("\n".join(lines))


def cpu_model():
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return "unknown CPU"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", default=os.environ.get(
        "CORRAL_BUILD_DIR", os.path.join(REPO, "build")))
    parser.add_argument("--latency-runs", type=int, default=30)
    parser.add_argument("--overhead-runs", type=int, default=200)
    parser.add_argument("--out", default=os.path.join(REPO, "bench", "results"))
    cfg = parser.parse_args()

    ce_parent, ce_reason = enforced_parent(cfg.build)
    modes = ["CE", "CF"] if ce_parent else ["CF"]
    corral = os.path.join(cfg.build, "corral")
    results = {"started_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
               "ce_unavailable": ce_reason, "latency": [], "overhead": []}

    for scenario, program in LATENCY_SCENARIOS:
        for mode in modes:
            row = summary(latency(cfg.build, mode, ce_parent, program, cfg.latency_runs))
            results["latency"].append({"scenario": scenario, "mode": mode, **row})
            print(f"latency {scenario} {mode}: {row}", file=sys.stderr, flush=True)

    runners = [("direct", ["/bin/true"])]
    for mode in modes:
        runners.append((mode, [corral, "--quiet", *mode_options(mode, ce_parent), "--",
                               "/bin/true"]))
    for name, argv in runners:
        row = summary(overhead(argv, cfg.overhead_runs))
        results["overhead"].append({"runner": name, **row})
        print(f"overhead {name}: {row}", file=sys.stderr, flush=True)

    info = machine()
    info["model"] = cpu_model()
    results["machine"] = info
    os.makedirs(cfg.out, exist_ok=True)
    with open(os.path.join(cfg.out, "bench.json"), "w") as f:
        json.dump(results, f, indent=2)
        f.write("\n")
    write_markdown(os.path.join(cfg.out, "bench.md"), results, info)
    print(open(os.path.join(cfg.out, "bench.md")).read())
    return 0


if __name__ == "__main__":
    sys.exit(main())
