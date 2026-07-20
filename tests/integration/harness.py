"""Helpers shared by the integration suite and bench/compare.py.

A surviving process is any process whose cmdline contains the run's TAG
200 ms after the runner under test returned or was killed.
"""

import os
import signal
import subprocess
import tempfile
import time
import uuid


CGROUP_ROOT = "/sys/fs/cgroup"
_runs_dir = None


def new_tag():
    return "corraltag-" + uuid.uuid4().hex


def own_cgroup():
    """This process's cgroup v2 directory, or None without cgroup v2."""
    with open("/proc/self/cgroup") as f:
        for line in f:
            if line.startswith("0::"):
                return CGROUP_ROOT + line[3:].strip().rstrip("/")
    return None


def enable_controllers(cgroup, controllers):
    for c in controllers:
        with open(os.path.join(cgroup, "cgroup.subtree_control"), "w") as f:
            f.write("+" + c)


def enforced_parent():
    """Prepares this process's delegated cgroup G for enforced runs.

    A cgroup that holds processes cannot enable controllers for its children,
    so this process moves into G/harness-leaf first. Returns G/runs, the parent
    for corral's boxes, or None if G is not delegated to this user.
    """
    global _runs_dir
    if _runs_dir is not None:
        return _runs_dir
    g = own_cgroup()
    if g is None or not os.access(g, os.W_OK):
        return None
    try:
        leaf = os.path.join(g, "harness-leaf")
        runs = os.path.join(g, "runs")
        os.makedirs(leaf, exist_ok=True)
        with open(os.path.join(leaf, "cgroup.procs"), "w") as f:
            f.write(str(os.getpid()))
        with open(os.path.join(g, "cgroup.controllers")) as f:
            available = f.read().split()
        wanted = [c for c in ("memory", "pids") if c in available]
        enable_controllers(g, wanted)
        os.makedirs(runs, exist_ok=True)
        enable_controllers(runs, wanted)
    except OSError:
        return None
    _runs_dir = runs
    return runs


def tagged_pids(tag):
    needle = tag.encode()
    pids = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            with open(f"/proc/{entry}/cmdline", "rb") as f:
                if needle in f.read():
                    pids.append(int(entry))
        except OSError:
            pass  # exited while we looked
    return pids


def kill_tagged(tag):
    pids = tagged_pids(tag)
    for pid in pids:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    return pids


class Result:
    def __init__(self, code, elapsed, hung, stdout, stderr, survivors):
        self.code = code
        self.elapsed = elapsed
        self.hung = hung
        self.stdout = stdout
        self.stderr = stderr
        self.survivors = survivors
        self.record = None


def run(argv, tag, signal_after=None, sig=signal.SIGINT, kill_after=30.0):
    """Runs one runner the way an agent does and counts the survivors.

    If the runner has not exited `kill_after` seconds after it started, it is
    killed with SIGKILL and the result is marked hung.
    """
    # stdin is a pipe that is never written, like an agent's inherited stdin.
    # stdout and stderr go to files, never to pipes the harness would wait on.
    stdin_r, stdin_w = os.pipe()
    try:
        with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
            start = time.monotonic()
            proc = subprocess.Popen(argv, stdin=stdin_r, stdout=out, stderr=err)
            os.close(stdin_r)
            stdin_r = -1
            hung = False
            try:
                if signal_after is not None:
                    time.sleep(signal_after)
                    proc.send_signal(sig)
                code = proc.wait(timeout=max(0.0, kill_after - (time.monotonic() - start)))
            except subprocess.TimeoutExpired:
                hung = True
                proc.kill()
                code = proc.wait()
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
            elapsed = time.monotonic() - start
            out.seek(0)
            err.seek(0)
            stdout, stderr = out.read(), err.read()
        time.sleep(0.2)
        # Scanned before stdin closes: EOF would let a leaked F2 exit on its own.
        survivors = tagged_pids(tag)
    finally:
        if stdin_r >= 0:
            os.close(stdin_r)
        os.close(stdin_w)
    return Result(code, elapsed, hung, stdout, stderr, survivors)
