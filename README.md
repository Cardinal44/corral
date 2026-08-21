# corral

corral runs one command with a time limit and guarantees, and then verifies,
that nothing the command started is still running when corral returns.

**corral is not a security sandbox.** It does not confine file access, network
access, or privileges. To isolate a command, run corral inside a sandbox such
as bubblewrap.

## The problem

AI agents and automation runners execute shell commands they did not write.
The usual pattern is "spawn a child, wait with a timeout, kill the child's
pid". It fails like this:

1. The runner starts `./build.sh` with a 60 s timeout.
2. The script starts a helper in the background (`server &`), or a tool
   double-forks and calls `setsid()` to become a daemon.
3. The script exits. The runner waits for EOF on the output pipe, but the
   background process inherited that pipe and holds it open. The runner hangs.
4. The timeout fires and the runner kills the one pid it knows. The helper has
   a different pid, and possibly a different process group and session. It
   survives, is re-parented to init, and keeps its ports, files, and CPU.
5. The next run fails because the port is taken, or the machine slowly fills
   with orphans.

POSIX's description of `timeout` states the root cause: descendants that
change their process group do not receive the signal.

Related public issues, with their state on 2026-08-20:

- anthropics/claude-code #45717, "[BUG] Bash tool timeout kills Claude Code
  process (SIGTERM propagation), not just the child command". Closed as not
  planned.
- anthropics/claude-code #43944, "Background processes started by Bash tool
  are not cleaned up on session exit". Closed as not planned.
- anthropics/claude-code #25188, "Background task cleanup kills long-running
  processes started via Bash tool". Closed as a duplicate. It describes the
  opposite failure: cleanup that reaches too far.
- openai/codex-plugin-cc #370, "Proposal: detach long-running codex-rescue
  dispatches via external bg-tmux helper (Bash tool 10-min ceiling
  workaround)". Open. A workaround proposal, not a bug report.
- containerd/containerd #4594, "Terminate a process group grandchildren on
  exec timeout". Open.

## The guarantee

When corral returns, every process that the command started is dead, and
corral has reaped all of its own children. corral checks this before it
returns. If it cannot prove it, it exits with **120** and lists the pids that
survived.

The two exceptions are exit 125 (the setup failed, so no command ran) and
exit 120 itself.

corral has two modes:

- **Enforced mode** puts the command in its own cgroup v2 directory, the
  *box*. The kernel tracks every descendant, whatever it does with process
  groups or sessions. corral kills the whole box with `cgroup.kill`, then
  checks that the kernel reports it empty and that `rmdir` of the box
  succeeds. This mode also enforces `--mem` and `--pids`.
- **Fallback mode** needs no cgroup. corral finds the command's processes by
  their parent, process group, and session, and scans `/proc` again and again
  until nothing is left. It covers double-forked daemons, because corral
  adopts every orphan in the tree. It cannot limit memory or processes.

The default, `--cgroup-mode=auto`, uses enforced mode when the cgroup is
delegated to the user, and falls back otherwise. The audit record always says
which mode ran and why.

## Quickstart

Requirements: Linux x86-64, kernel 5.11 or newer (5.14 or newer for enforced
mode), glibc 2.36 or newer, CMake 3.22+, Ninja, GCC 11+ or Clang 14+, and
Python 3.10+ for the tests. No other dependencies.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Run a command:

```sh
./build/corral --wall 30s -- ./build.sh
./build/corral --wall 5m --max-output 1M --json run.json -- make test
```

Enforced mode needs a delegated cgroup. With systemd, the wrapper starts corral
in a new delegated scope:

```sh
./scripts/corral-enforced --wall 30s --mem 512M --pids 64 -- ./build.sh
```

The wrapper uses `build/corral`; set `CORRAL_BIN` to use another binary.

Longer checks:

```sh
CORRAL_RACE=1 CORRAL_BUILD_DIR=$PWD/build bash scripts/run-integration.sh
systemd-run --user --scope -p Delegate=yes --quiet python3 bench/compare.py --build build
systemd-run --user --scope -p Delegate=yes --quiet python3 bench/bench.py --build build
./scripts/demo.sh
```

## Command line

```text
corral [options] [--] command [args...]
```

| Option | Default | Meaning |
|---|---|---|
| `--wall DURATION` | none | Wall-clock limit for the run. |
| `--grace DURATION` | `2s` | Time between SIGTERM and SIGKILL. `0` skips SIGTERM. |
| `--verify-timeout DURATION` | `2s` | Maximum time to drain and verify after SIGKILL. |
| `--mem SIZE` | none | Memory limit (enforced mode only). |
| `--pids COUNT` | none | Maximum number of processes (enforced mode only). |
| `--max-output SIZE` | none | Cap on stdout plus stderr, in bytes. |
| `--cgroup-mode MODE` | `auto` | `auto`, `enforced`, or `fallback`. |
| `--cgroup-parent PATH` | none | A delegated cgroup under which to create boxes. |
| `--stdin POLICY` | `null` | `null` (`/dev/null`) or `inherit`. |
| `--json PATH` | none | Write the audit record to PATH. `/dev/fd/N` works. |
| `--quiet` | off | Do not print the summary line. |
| `--version`, `--help` | | Print and exit. |

DURATION is an integer with `ms`, `s`, `m`, or `h` (a bare integer is
seconds). SIZE is an integer with an optional `K`, `M`, or `G` (powers of
1024).

The command's stdout and stderr go to corral's stdout and stderr. corral's
own messages, and a one-line summary at the end, go to stderr:

```text
corral: timeout code=124 in 2.003s mode=fallback leftovers_killed=1
```

### Exit codes

| Code | Meaning |
|---|---|
| the command's code | It exited by itself. |
| 128 + N | It was killed by signal N, which corral did not send. |
| 124 | The `--wall` deadline passed. |
| 121 | The memory limit was hit (enforced mode). |
| 122 | The output cap was hit. |
| 128 + N | corral received signal N: 130 for SIGINT, 143 for SIGTERM, 129 for SIGHUP. |
| 126, 127 | The command could not be executed; 127 means not found. |
| 125 | Usage error, or setup failed (for example `--cgroup-mode=enforced` without delegation). |
| 120 | Verification failed: something survived, or corral could not prove that nothing did. |

Exit codes overlap with the command's own codes. The audit record is the
authoritative source.

## The audit record

With `--json PATH`, corral writes one JSON object per run. This record is from
the test machine: fault F5 (a daemon whose launcher stays) in enforced mode,
stopped by the 2 s deadline.

```json
{
  "schema_version": 1,
  "corral_version": "0.1.0",
  "argv": ["build-rel/faults/corral_daemonizer_stay", "readme"],
  "mode": "enforced",
  "mode_reason": null,
  "limits": {
    "wall_ms": 2000, "grace_ms": 1000, "verify_timeout_ms": 2000,
    "mem_bytes": null, "pids": null, "output_bytes": null
  },
  "limits_enforced": { "wall": true, "mem": null, "pids": null, "output": null },
  "started_at": "2026-08-21T03:36:10.650Z",
  "duration_ms": 2011,
  "exit_reason": "timeout",
  "exit_code": null,
  "term_signal": 15,
  "cancel_signal": null,
  "spawn_errno": null,
  "corral_exit_code": 124,
  "output": { "stdout_bytes": 0, "stderr_bytes": 0, "truncated": false },
  "termination": {
    "trigger_ms": 2002, "leftovers_at_trigger": 1,
    "term_sent_ms": 2005, "kill_sent_ms": null, "verified_ms": 2010
  },
  "orphans_reaped": 1,
  "orphans_remaining": 0,
  "survivors": [],
  "limit_events": { "oom_kills": 0, "pids_max_hits": 0, "peak_mem_bytes": 786432 },
  "cgroup_path": "/sys/fs/cgroup/user.slice/user-1000.slice/user@1000.service/app.slice/run-p94671-i90024.scope/corral-run-94671-1",
  "kernel": "7.0.0-31-generic"
}
```

corral writes the record on one line; it is shown formatted here. All `_ms`
values are milliseconds since the start of the run, from a monotonic clock.
In `limits_enforced`, `null` means "not requested" and `false` means
"requested but not applied", so the record never claims a limit that did not
hold. `leftovers_at_trigger` counts the processes, other than the command
itself, that were alive when the run ended: here, the daemon.

## How it works

- **Session isolation.** The child calls `setsid()` before exec, so corral
  never shares a process group or session with the command. A signal to the
  command's group can never reach corral.
- **Start inside the box.** The child waits on a pipe before it execs. corral
  moves it into the box during that pause, so every descendant is born inside
  the box.
- **Subreaper.** corral calls `PR_SET_CHILD_SUBREAPER`, so orphaned
  descendants are re-parented to corral instead of init. corral can then find,
  kill, and reap them.
- **pidfds.** corral watches the child through a pidfd in the same `poll()`
  loop as the output pipes. It signals processes that are not its children only
  through a pidfd, after it checks again that the pid still belongs to the run.
  A recycled pid can never receive the signal.
- **`WNOWAIT`.** corral reads the child's exit status without reaping it. The
  pid and the process group id stay reserved while corral still signals the
  group.
- **No wait for EOF.** The run ends when the main child exits, not when the
  pipes close. A background process that holds the output pipe cannot hang
  corral.
- **`cgroup.kill`.** In enforced mode, one write kills every process in the
  box, also a child that a racing `fork()` creates.
- **Verification.** After every run, even a clean exit, corral kills and
  reaps until nothing is alive and `waitpid` reports that corral has no
  children. In enforced mode the kernel must also report the box empty, and
  `rmdir` must succeed.

## Evaluation

`bench/compare.py` runs ten fault programs (in `faults/`) under four runners:

- **CE**: corral, enforced mode.
- **CF**: corral, fallback mode.
- **TO**: `timeout -k 1s 2s`.
- **NV**: `bench/naive_runner.py`, the common agent pattern:
  `Popen(stdout=PIPE, stderr=PIPE)`, `communicate(timeout=2)`, and on timeout
  `kill()` and `communicate()` again. NV is killed after 7 s and marked `hung`.

| ID | Fault program | Behavior |
|---|---|---|
| F1 | `corral_streamer` | Writes a line every 100 ms, forever |
| F2 | `corral_stdin_waiter` | Waits for stdin until EOF |
| F3 | `corral_script_spawner.sh` | Starts a background job, exits 0 |
| F4 | `corral_daemonizer` | Double-forks a `setsid()` daemon, exits 0 |
| F5 | `corral_daemonizer_stay` | Like F4, but the launcher stays |
| F6 | `corral_term_ignorer` | Ignores SIGTERM |
| F7 | `corral_forker` | Forks up to 200 children (CE adds `--pids 32`) |
| F8 | `corral_pipe_holder` | A child holds stdout; the parent exits 0 |
| F9 | `corral_mem_hog` | Allocates 256 MiB (CE adds `--mem 64M`) |
| F10 | `corral_graceful` | Prints `cleaning up` on SIGTERM, then exits |

All runs use `--wall 2s --grace 1s --verify-timeout 2s`. Each cell shows the
runner's exit status, the worst survivor count over 5 repetitions, and the
median duration:

| ID | CE | CF | TO | NV |
|---|---|---|---|---|
| F1 | 124; 0 surv; 2.02 s | 124; 0 surv; 2.02 s | 124; 0 surv; 2.12 s | 137; 0 surv; 2.07 s |
| F2 | 0; 0 surv; 0.03 s | 0; 0 surv; 0.02 s | 124; 0 surv; 2.12 s | 137; 0 surv; 2.07 s |
| F3 | 0; 0 surv; 0.07 s | 0; 0 surv; 0.03 s | 0; 1 surv; 0.12 s | hung; 1 surv; 7.00 s |
| F4 | 0; 0 surv; 0.03 s | 0; 0 surv; 0.02 s | 0; 1 surv; 0.12 s | hung; 1 surv; 7.00 s |
| F5 | 124; 0 surv; 2.02 s | 124; 0 surv; 2.02 s | 124; 1 surv; 2.12 s | hung; 1 surv; 7.00 s |
| F6 | 124; 0 surv; 3.03 s | 124; 0 surv; 3.03 s | 137; 0 surv; 3.03 s | 137; 0 surv; 2.07 s |
| F7 | 124; 0 surv; 2.02 s | 124; 0 surv; 2.08 s | 124; 1 surv; 2.12 s | hung; 192 surv; 7.00 s |
| F8 | 0; 0 surv; 0.03 s | 0; 0 surv; 0.02 s | 0; 1 surv; 0.12 s | hung; 1 surv; 7.00 s |
| F9 | 121; 0 surv; 0.12 s | 124; 0 surv; 2.07 s | 124; 0 surv; 2.12 s | 137; 0 surv; 2.12 s |
| F10 | 124; 0 surv; 2.02 s; cleanup output | 124; 0 surv; 2.02 s; cleanup output | 124; 0 surv; 2.12 s; cleanup output | 137; 0 surv; 2.07 s |

Result: **corral left zero survivors in every scenario, in both modes. TO left
a survivor in 5 scenarios. NV hung in 5 and left survivors in each of them.**

Notes on the table:

- TO's survivor in F7 appeared in 1 of the 5 runs; the other four left none.
- F9 under CE ends in 0.12 s with exit 121: the kernel OOM-killed the box at
  64 MiB. Without a memory limit, the other runners let it run to the deadline.
- On this machine, `timeout` is the uutils coreutils 0.8.0 version that Ubuntu
  26.04 installs by default, not GNU coreutils.

The raw data is in `bench/results/results.json`. A terminal recording of
`scripts/demo.sh`, which runs F5 under NV, TO, and corral and lists what each
leaves alive, is in `bench/results/demo.cast` (`asciinema play
bench/results/demo.cast`).

### Performance

`bench/bench.py` measures termination latency: the time from the trigger (here
the deadline) until corral has verified that nothing is left
(`verified_ms - trigger_ms` in the record). F6 ignores SIGTERM, so its latency
includes the 1 s grace period before SIGKILL.

| Scenario | Mode | P50 (ms) | P95 (ms) | Runs |
|---|---|---|---|---|
| F5 | CE | 9 | 12 | 30 |
| F5 | CF | 10 | 12 | 30 |
| F6 | CE | 1011 | 1013 | 30 |
| F6 | CF | 1010 | 1014 | 30 |
| F10 | CE | 9 | 12 | 30 |
| F10 | CF | 8 | 12 | 30 |

It also measures overhead: the wall time of one run of `/bin/true`, started
from Python, including process creation. CE runs inside an existing delegated
scope, so `systemd-run` startup is not included.

| Runner | P50 (ms) | P95 (ms) | Runs |
|---|---|---|---|
| direct | 0.83 | 1.88 | 200 |
| CF | 6.64 | 10.01 | 200 |
| CE | 11.33 | 19.47 | 200 |

The raw data is in `bench/results/bench.json`.

### Test machine

Hetzner cloud server, 2 shared vCPUs (Intel Xeon Processor, Skylake, IBRS, no
TSX), x86-64, Ubuntu 26.04 LTS, kernel 7.0.0-31-generic, GCC 15.2, Python
3.14. The same machine passed the full test suite in both modes, race mode
(every scenario 20 times), and the valgrind check.

Shared vCPUs make the timings noisy, and 2 CPUs allow less parallelism than a
larger machine. The survivor counts do not depend on timing.

## Limitations

- **Not a sandbox.** No file, network, or privilege confinement.
- **Work outside the process tree** is out of scope in both modes, for example
  work started through `systemd-run`, D-Bus activation, `at`, or a container
  runtime.
- **A process that moves itself out of the box** by writing to cgroupfs is out
  of scope. Delegation gives the user that write access.
- **A setuid descendant** can be impossible to signal in fallback mode. corral
  then exits 120. Enforced mode kills it through `cgroup.kill`.
- **If corral itself is killed with SIGKILL,** only the main child is
  guaranteed to die (through `PR_SET_PDEATHSIG`). In fallback mode descendants
  can leak. In enforced mode they stay in the box until something kills it.
- No PTY support, no CPU time limit, Linux only.

## Design notes and deferred work

- The library (`corralcore`) never prints and never installs signal handlers.
  The CLI turns SIGINT, SIGTERM, and SIGHUP into a signalfd, and the library
  treats that fd as "cancel".
- `run()` is single-threaded and assumes that the calling process has no other
  children while it runs, because it reaps with `waitpid(-1)`.
- Deferred: `clone3` with `CLONE_INTO_CGROUP` (atomic spawn into the box), CPU
  time limits, PID namespaces and seccomp, PTY allocation, freezing the box
  before the kill, concurrent runs from one process, and non-blocking
  forwarding to a slow stdout consumer.
