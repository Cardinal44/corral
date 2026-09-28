# corral

corral runs a command with a time limit. When corral returns, no process that
the command started is still alive, and corral checks this before it returns.

I made corral for AI coding agents and CI jobs. They run shell commands that
they did not write, and their runners usually stop only the process that they
started. Background jobs, double-forked daemons, and child processes that hold
the output pipe then continue to run, or they make the runner hang. Leftover
processes keep ports, files, and CPU, and the next run can fail because of
them. corral finds and stops all of them.

corral is not a security sandbox. It does not limit file access, network
access, or privileges.

## Install

corral needs Linux 5.11 or later, and 5.14 or later for enforced mode.

The prebuilt binary is for x86-64 with glibc 2.36 or later (for example Ubuntu
22.10, Debian 12, or Fedora 37, or a later release):

```sh
curl -LO https://github.com/Cardinal44/corral/releases/latest/download/corral-linux-x86_64.tar.gz
tar -xzf corral-linux-x86_64.tar.gz
sudo cp corral-linux-x86_64/corral corral-linux-x86_64/corral-enforced /usr/local/bin/
```

To build from source, you need glibc 2.36, CMake 3.22, Ninja, and GCC 11 or
Clang 14. The tests need Python 3.10.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## Use

```sh
corral --wall 30s -- ./build.sh
corral --wall 5m --max-output 1M --json run.json -- make test
corral-enforced --wall 30s --mem 512M --pids 64 -- ./build.sh
```

corral has two modes:

- **Enforced mode** puts the command in its own cgroup v2 group. The kernel
  keeps every descendant in that group, and one write to `cgroup.kill` stops
  all of them. This mode also applies `--mem` and `--pids`. It needs a
  delegated cgroup. `scripts/corral-enforced` starts one with `systemd-run`.
- **Fallback mode** needs no cgroup. corral finds the processes by parent,
  process group, and session, and scans `/proc` until nothing is left.

The default, `--cgroup-mode=auto`, uses enforced mode when it can.

| Option | Default | Meaning |
|---|---|---|
| `--wall DURATION` | none | Time limit for the run. |
| `--grace DURATION` | `2s` | Time between SIGTERM and SIGKILL. |
| `--verify-timeout DURATION` | `2s` | Time limit for the final check. |
| `--mem SIZE` | none | Memory limit (enforced mode). |
| `--pids COUNT` | none | Process limit (enforced mode). |
| `--max-output SIZE` | none | Limit for stdout and stderr together. |
| `--cgroup-mode MODE` | `auto` | `auto`, `enforced`, or `fallback`. |
| `--cgroup-parent PATH` | none | Delegated cgroup for the groups that corral makes. |
| `--stdin POLICY` | `null` | `null` or `inherit`. |
| `--json PATH` | none | Write the audit record to PATH. |
| `--quiet` | off | Do not print the summary line. |

A DURATION is a number with `ms`, `s`, `m`, or `h`. A SIZE is a number of bytes
with an optional `K`, `M`, or `G`.

With `--json`, corral writes one JSON record for each run. The record gives the
mode, the limits, and why the run ended. It also gives the time of each step
and the pids of any process that did not stop.

## Exit codes

| Code | Meaning |
|---|---|
| command's code | The command exited. |
| 128 + N | Signal N stopped the command, or corral received signal N (130 for Ctrl-C). |
| 124 | The time limit expired. |
| 121 | The memory limit was reached. |
| 122 | The output limit was reached. |
| 126, 127 | corral could not start the command (127: not found). |
| 125 | Setup failed or an option is not correct. The command did not start. |
| 120 | corral could not prove that all processes stopped. This code overrides all other codes. |

## How it works

- The command starts in a new session, so a signal to its group never reaches
  corral.
- corral is a child subreaper. Orphaned processes become children of corral,
  so corral can stop them.
- corral signals a process that is not its child only through a pidfd, after
  it checks the process again. A reused pid never gets the signal.
- The run ends when the command exits, not when its output pipes close.
- After each run, corral stops and reaps processes until none are left. In
  enforced mode, the kernel must also report the group empty, and `rmdir` of
  the group must succeed.

## Results

`bench/compare.py` runs ten test programs from `faults/` with four runners:

- CE: corral in enforced mode
- CF: corral in fallback mode
- TO: `timeout -k 1s 2s`
- NV: a Python runner that kills only its child

Each run has a 2 s time limit. Each cell shows the exit status and the largest number of processes
that stayed alive, over 5 runs.

| Test program | CE | CF | TO | NV |
|---|---|---|---|---|
| F1 prints a line every 100 ms | 124 / 0 | 124 / 0 | 124 / 0 | 137 / 0 |
| F2 waits for stdin | 0 / 0 | 0 / 0 | 124 / 0 | 137 / 0 |
| F3 starts a background job | 0 / 0 | 0 / 0 | 0 / 1 | hung / 1 |
| F4 starts a daemon and exits | 0 / 0 | 0 / 0 | 0 / 1 | hung / 1 |
| F5 starts a daemon and stays | 124 / 0 | 124 / 0 | 124 / 1 | hung / 1 |
| F6 ignores SIGTERM | 124 / 0 | 124 / 0 | 137 / 0 | 137 / 0 |
| F7 forks 200 children | 124 / 0 | 124 / 0 | 124 / 1 | hung / 192 |
| F8 child holds stdout | 0 / 0 | 0 / 0 | 0 / 1 | hung / 1 |
| F9 uses 256 MiB | 121 / 0 | 124 / 0 | 124 / 0 | 137 / 0 |
| F10 cleans up on SIGTERM | 124 / 0 | 124 / 0 | 124 / 0 | 137 / 0 |

corral left no process alive in any test. CE also stopped F9 at its 64 MiB
limit in 0.12 s. TO left a process alive in F7 in 1 of 5 runs. On this machine,
`timeout` is from uutils coreutils 0.8.0, not GNU coreutils.

`bench/bench.py` measured these times (P50 / P95):

| Measurement | Enforced | Fallback |
|---|---|---|
| Stop F5: time limit to verified | 9 / 12 ms | 10 / 12 ms |
| Run `/bin/true` (direct: 0.83 / 1.88 ms) | 11.33 / 19.47 ms | 6.64 / 10.01 ms |

Test machine: Hetzner cloud server, 2 shared vCPUs (Intel Xeon, Skylake),
Ubuntu 26.04, kernel 7.0. The full data and a terminal recording of
`scripts/demo.sh` are in `bench/results/`.

## Limits

- corral cannot see work that the command starts outside its process tree,
  for example with `systemd-run`, D-Bus, or `at`.
- In enforced mode, a process can leave the group if it writes to cgroupfs
  itself.
- In fallback mode, corral cannot signal a setuid child. It then exits with 120.
- If you stop corral with SIGKILL, only the direct child is sure to stop.
- corral has no PTY support and no CPU time limit, and it runs only on Linux.

## License

MIT. See [LICENSE](LICENSE).
