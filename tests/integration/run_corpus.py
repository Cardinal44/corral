"""Integration tests against the real corral binary.

Each scenario gets a fresh TAG. A surviving process is any process whose
cmdline contains the TAG 200 ms after corral returned.
"""

import os
import signal
import subprocess
import tempfile
import time
import unittest
import uuid

BUILD = os.environ.get(
    "CORRAL_BUILD_DIR", os.path.join(os.path.dirname(__file__), "..", "..", "build"))
CORRAL = os.path.join(BUILD, "corral")
FAULTS = os.path.join(BUILD, "faults")
STANDARD = ["--wall", "2s", "--grace", "1s", "--verify-timeout", "2s"]


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


class Run:
    def __init__(self, code, elapsed, stdout, stderr, survivors):
        self.code = code
        self.elapsed = elapsed
        self.stdout = stdout
        self.stderr = stderr
        self.survivors = survivors


def run_corral(args, tag, signal_after=None, sig=signal.SIGINT):
    # stdin is a pipe that is never written, like an agent's inherited stdin.
    # stdout and stderr go to files, never to pipes the harness would wait on.
    stdin_r, stdin_w = os.pipe()
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        start = time.monotonic()
        proc = subprocess.Popen([CORRAL, "--cgroup-mode=fallback", "--quiet", *args],
                                stdin=stdin_r, stdout=out, stderr=err)
        os.close(stdin_r)
        try:
            if signal_after is not None:
                time.sleep(signal_after)
                proc.send_signal(sig)
            code = proc.wait(timeout=30)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(stdin_w)
        elapsed = time.monotonic() - start
        out.seek(0)
        err.seek(0)
        stdout, stderr = out.read(), err.read()
    time.sleep(0.2)
    return Run(code, elapsed, stdout, stderr, tagged_pids(tag))


class FallbackCorpus(unittest.TestCase):
    def scenario(self, program, *program_args, options=STANDARD, **kwargs):
        tag = "corraltag-" + uuid.uuid4().hex
        argv = [os.path.join(FAULTS, program), tag, *program_args]
        try:
            run = run_corral([*options, "--", *argv], tag, **kwargs)
        finally:
            leaked = kill_tagged(tag)
        self.assertEqual(run.survivors, [], f"stderr: {run.stderr!r}")
        self.assertEqual(leaked, [], "cleanup found tagged processes")
        # None of these fault programs write to stderr, so anything there is
        # a corral diagnostic.
        self.assertEqual(run.stderr, b"")
        return run

    def test_wall_timeout(self):
        run = self.scenario("corral_sleeper", "100", options=["--wall", "1s"])
        self.assertEqual(run.code, 124)
        self.assertGreaterEqual(run.elapsed, 1.0)
        self.assertLessEqual(run.elapsed, 1.5)

    def test_term_ignorer_escalates_to_sigkill(self):
        run = self.scenario("corral_term_ignorer", options=["--wall", "1s", "--grace", "1s"])
        self.assertEqual(run.code, 124)
        self.assertGreaterEqual(run.elapsed, 2.0)
        self.assertLessEqual(run.elapsed, 2.6)

    def test_f3_script_background_job(self):
        self.assertEqual(self.scenario("corral_script_spawner.sh").code, 0)

    def test_f4_daemonizer(self):
        self.assertEqual(self.scenario("corral_daemonizer").code, 0)

    def test_f5_daemonizer_stay(self):
        self.assertEqual(self.scenario("corral_daemonizer_stay").code, 124)

    def test_f8_pipe_holder(self):
        run = self.scenario("corral_pipe_holder")
        self.assertEqual(run.code, 0)
        self.assertEqual(run.stdout, b"hello\n")
        self.assertLess(run.elapsed, 1.5)

    def test_sigint_cancels_f5(self):
        run = self.scenario("corral_daemonizer_stay", options=["--grace", "1s"],
                            signal_after=0.5)
        self.assertEqual(run.code, 130)

    def test_sigterm_cancels_f5(self):
        run = self.scenario("corral_daemonizer_stay", options=["--grace", "1s"],
                            signal_after=0.5, sig=signal.SIGTERM)
        self.assertEqual(run.code, 143)


if __name__ == "__main__":
    unittest.main()
