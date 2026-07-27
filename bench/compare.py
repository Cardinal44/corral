#!/usr/bin/env python3
"""Baseline comparison.

Runs the ten fault scenarios under corral enforced (CE), corral fallback (CF),
GNU `timeout -k 1s 2s` (TO), and bench/naive_runner.py (NV). It counts the
survivors of every run, writes results.json and results.md, and evaluates the
claim: corral leaves zero survivors in every scenario, while TO and NV each
leave a survivor or hang in at least one.

CE needs a delegated cgroup, so run it inside one systemd scope:
    systemd-run --user --scope -p Delegate=yes --quiet python3 bench/compare.py
"""

import argparse
import collections
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(REPO, "tests", "integration"))
import harness  # noqa: E402  (needs the path above)

SCENARIOS = [
    ("F1", "corral_streamer", []),
    ("F2", "corral_stdin_waiter", []),
    ("F3", "corral_script_spawner.sh", []),
    ("F4", "corral_daemonizer", []),
    ("F5", "corral_daemonizer_stay", []),
    ("F6", "corral_term_ignorer", []),
    ("F7", "corral_forker", []),
    ("F8", "corral_pipe_holder", []),
    ("F9", "corral_mem_hog", ["256"]),
    ("F10", "corral_graceful", []),
]
RUNNERS = ["CE", "CF", "TO", "NV"]
STANDARD = ["--wall", "2s", "--grace", "1s", "--verify-timeout", "2s"]
NV_KILL_AFTER = 7.0
# corral and TO bound themselves; this only stops a broken runner.
KILL_AFTER = 30.0


def runner_argv(runner, cfg, scenario, command):
    if runner == "CE":
        extra = ["--cgroup-mode=enforced", "--cgroup-parent", cfg.ce_parent]
        if scenario == "F7":
            extra += ["--pids", "32"]
        if scenario == "F9":
            extra += ["--mem", "64M"]
        return [os.path.join(cfg.build, "corral"), "--quiet", *extra, *STANDARD, "--", *command]
    if runner == "CF":
        return [os.path.join(cfg.build, "corral"), "--quiet", "--cgroup-mode=fallback",
                *STANDARD, "--", *command]
    if runner == "TO":
        return [cfg.timeout, "-k", "1s", "2s", *command]
    return [sys.executable, os.path.join(REPO, "bench", "naive_runner.py"), *command]


def enforced_parent(build):
    """The box parent for CE, and None or the reason it is not available."""
    parent = harness.enforced_parent()
    if parent is None:
        return None, "no delegated cgroup; run inside systemd-run --user --scope -p Delegate=yes"
    probe = subprocess.run(
        [os.path.join(build, "corral"), "--cgroup-mode=enforced", "--cgroup-parent", parent,
         "--quiet", "--", "true"],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if probe.returncode != 0:
        return None, probe.stderr.decode(errors="replace").strip()
    return parent, None


def measure(cfg, runners, reps):
    rows = []
    for scenario, program, args in SCENARIOS:
        for runner in runners:
            for rep in range(reps):
                tag = harness.new_tag()
                command = [os.path.join(cfg.build, "faults", program), tag, *args]
                try:
                    r = harness.run(runner_argv(runner, cfg, scenario, command), tag,
                                    kill_after=NV_KILL_AFTER if runner == "NV" else KILL_AFTER)
                finally:
                    harness.kill_tagged(tag)
                if r.hung:
                    code = None
                else:
                    # Shell convention, as corral and naive_runner.py report it.
                    code = r.code if r.code >= 0 else 128 - r.code
                rows.append({
                    "scenario": scenario, "runner": runner, "rep": rep,
                    "exit_code": code, "hung": r.hung,
                    "duration_s": round(r.elapsed, 3), "survivors": len(r.survivors),
                    "cleanup_output": b"cleaning up" in r.stdout,
                })
            print(f"{scenario:>3} {runner}: {cell(select(rows, scenario, runner))}",
                  file=sys.stderr, flush=True)
    return rows


def select(rows, scenario, runner):
    return [r for r in rows if r["scenario"] == scenario and r["runner"] == runner]


def cell(runs):
    """One table cell: exit statuses, worst survivor count, median duration."""
    if not runs:
        return "not run"
    counts = collections.Counter("hung" if r["hung"] else str(r["exit_code"]) for r in runs)
    status = ", ".join(s if n == len(runs) else f"{s} x{n}" for s, n in counts.most_common())
    text = (f"{status}; {max(r['survivors'] for r in runs)} surv; "
            f"{statistics.median(r['duration_s'] for r in runs):.2f} s")
    if all(r["cleanup_output"] for r in runs):
        text += "; cleanup output"
    return text


def evaluate(rows, runners):
    """Returns the failing cells; empty means the claim holds."""
    failures = []
    for runner in ("CE", "CF"):
        if runner not in runners:
            continue
        for r in rows:
            if r["runner"] == runner and (r["survivors"] or r["hung"]):
                failures.append(f"{runner} {r['scenario']} rep {r['rep']}: "
                                f"{r['survivors']} survivors" + (", hung" if r["hung"] else ""))
    for runner in ("TO", "NV"):
        if not any(r["survivors"] or r["hung"] for r in rows if r["runner"] == runner):
            failures.append(f"{runner}: no scenario left a survivor or hung")
    return failures


def machine():
    distro = ""
    try:
        with open("/etc/os-release") as f:
            for line in f:
                if line.startswith("PRETTY_NAME="):
                    distro = line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    return {"kernel": platform.release(), "arch": platform.machine(),
            "cpus": os.cpu_count(), "distribution": distro}


def write_markdown(path, rows, runners, verdict, info):
    lines = [
        "# Baseline comparison",
        "",
        f"Machine: {info['distribution']}, kernel {info['kernel']}, {info['arch']}, "
        f"{info['cpus']} CPUs.",
        "",
        f"TO is `{info['timeout']}`.",
        "",
        "Each cell: exit status of the runner (`hung` if it was killed after "
        f"{NV_KILL_AFTER:.0f} s), the worst survivor count over the repetitions, "
        "and the median duration.",
        "",
        "| ID | " + " | ".join(RUNNERS) + " |",
        "|---|" + "---|" * len(RUNNERS),
    ]
    for scenario, _, _ in SCENARIOS:
        cells = [cell(select(rows, scenario, r)) if r in runners else "not run"
                 for r in RUNNERS]
        lines.append(f"| {scenario} | " + " | ".join(cells) + " |")
    lines += ["", verdict, ""]
    with open(path, "w") as f:
        f.write("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", default=os.environ.get(
        "CORRAL_BUILD_DIR", os.path.join(REPO, "build")))
    parser.add_argument("--reps", type=int, default=5)
    parser.add_argument("--out", default=os.path.join(REPO, "bench", "results"))
    parser.add_argument("--timeout", default="timeout",
                        help="the timeout(1) program for TO (default: timeout on PATH)")
    cfg = parser.parse_args()
    timeout_path = shutil.which(cfg.timeout)
    if timeout_path is None:
        sys.exit(f"compare.py: {cfg.timeout} not found")
    # Not every distribution ships GNU coreutils, so the table names the one used.
    version = subprocess.run([timeout_path, "--version"], capture_output=True, text=True)
    timeout_version = (version.stdout.splitlines() or ["unknown"])[0]

    cfg.ce_parent, ce_reason = enforced_parent(cfg.build)
    runners = [r for r in RUNNERS if r != "CE" or cfg.ce_parent]
    started = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    rows = measure(cfg, runners, cfg.reps)
    failures = evaluate(rows, runners)

    if failures:
        verdict = "CLAIM FAILS\n" + "\n".join("  " + f for f in failures)
    elif cfg.ce_parent:
        verdict = "CLAIM HOLDS"
    else:
        verdict = f"CLAIM HOLDS for CF, TO, NV (CE not measured: {ce_reason})"

    info = machine()
    info["timeout"] = timeout_version
    os.makedirs(cfg.out, exist_ok=True)
    with open(os.path.join(cfg.out, "results.json"), "w") as f:
        json.dump({"started_at": started, "machine": info, "reps": cfg.reps,
                   "runners": runners, "ce_unavailable": ce_reason,
                   "runs": rows, "claim_holds": not failures, "failures": failures},
                  f, indent=2)
        f.write("\n")
    write_markdown(os.path.join(cfg.out, "results.md"), rows, runners, verdict, info)
    print(verdict)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
