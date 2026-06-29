"""Integration tests against the real corral binary.

Each scenario gets a fresh TAG. A surviving process is any process whose
cmdline contains the TAG 200 ms after corral returned.
"""

import json
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

# Audit record keys, in order.
RECORD_FIELDS = [
    "schema_version", "corral_version", "argv", "mode", "mode_reason", "limits",
    "limits_enforced", "started_at", "duration_ms", "exit_reason", "exit_code",
    "term_signal", "cancel_signal", "spawn_errno", "corral_exit_code", "output",
    "termination", "orphans_reaped", "orphans_remaining", "survivors", "limit_events",
    "cgroup_path", "kernel",
]
NESTED_FIELDS = {
    "limits": ["wall_ms", "grace_ms", "verify_timeout_ms", "mem_bytes", "pids", "output_bytes"],
    "limits_enforced": ["wall", "mem", "pids", "output"],
    "output": ["stdout_bytes", "stderr_bytes", "truncated"],
    "termination": ["trigger_ms", "leftovers_at_trigger", "term_sent_ms", "kill_sent_ms",
                    "verified_ms"],
    "limit_events": ["oom_kills", "pids_max_hits", "peak_mem_bytes"],
}


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
    def __init__(self, code, elapsed, stdout, stderr, record, survivors):
        self.code = code
        self.elapsed = elapsed
        self.stdout = stdout
        self.stderr = stderr
        self.record = record
        self.survivors = survivors


def run_corral(args, tag, signal_after=None, sig=signal.SIGINT):
    # stdin is a pipe that is never written, like an agent's inherited stdin.
    # stdout and stderr go to files, never to pipes the harness would wait on.
    stdin_r, stdin_w = os.pipe()
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err, \
            tempfile.TemporaryDirectory() as tmp:
        json_path = os.path.join(tmp, "record.json")
        start = time.monotonic()
        proc = subprocess.Popen([CORRAL, "--cgroup-mode=fallback", "--quiet",
                                 "--json", json_path, *args],
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
        with open(json_path, "rb") as f:
            raw = f.read()
    time.sleep(0.2)
    return Run(code, elapsed, stdout, stderr, raw, tagged_pids(tag))


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
        run.record = self.check_record(run.record, run.code, argv)
        return run

    def check_record(self, raw, code, argv):
        self.assertTrue(raw.endswith(b"}\n"), raw)
        self.assertEqual(raw.count(b"\n"), 1, "the record is one line")
        record = json.loads(raw)
        self.assertEqual(list(record), RECORD_FIELDS)
        for name, fields in NESTED_FIELDS.items():
            self.assertEqual(list(record[name]), fields, name)
        self.assertEqual(record["schema_version"], 1)
        self.assertEqual(record["argv"], argv)
        self.assertEqual(record["mode"], "fallback")
        self.assertEqual(record["mode_reason"], "forced_by_flag")
        self.assertEqual(record["corral_exit_code"], code)
        self.assertEqual(record["orphans_remaining"], 0)
        self.assertEqual(record["survivors"], [])
        self.assertEqual(record["limit_events"],
                         {"oom_kills": None, "pids_max_hits": None, "peak_mem_bytes": None})
        self.assertIsNone(record["cgroup_path"])
        self.assertEqual(record["kernel"], os.uname().release)
        return record

    def test_wall_timeout(self):
        run = self.scenario("corral_sleeper", "100", options=["--wall", "1s"])
        self.assertEqual(run.code, 124)
        self.assertGreaterEqual(run.elapsed, 1.0)
        self.assertLessEqual(run.elapsed, 1.5)
        self.assertEqual(run.record["exit_reason"], "timeout")
        self.assertEqual(run.record["term_signal"], signal.SIGTERM)
        self.assertEqual(run.record["limits"]["wall_ms"], 1000)
        self.assertEqual(run.record["limits_enforced"]["wall"], True)
        self.assertIsNone(run.record["termination"]["kill_sent_ms"])

    def test_f1_max_output(self):
        run = self.scenario("corral_streamer", options=["--max-output", "1000"])
        self.assertEqual(run.code, 122)
        self.assertEqual(len(run.stdout), 1000)
        self.assertEqual(run.record["exit_reason"], "output_limit")
        self.assertEqual(run.record["output"],
                         {"stdout_bytes": 1000, "stderr_bytes": 0, "truncated": True})
        self.assertEqual(run.record["limits"]["output_bytes"], 1000)
        self.assertEqual(run.record["limits_enforced"]["output"], True)

    def test_term_ignorer_escalates_to_sigkill(self):
        run = self.scenario("corral_term_ignorer", options=["--wall", "1s", "--grace", "1s"])
        self.assertEqual(run.code, 124)
        self.assertGreaterEqual(run.elapsed, 2.0)
        self.assertLessEqual(run.elapsed, 2.6)
        self.assertEqual(run.record["term_signal"], signal.SIGKILL)
        self.assertIsNotNone(run.record["termination"]["kill_sent_ms"])

    def test_f3_script_background_job(self):
        run = self.scenario("corral_script_spawner.sh")
        self.assertEqual(run.code, 0)
        self.assertEqual(run.record["exit_reason"], "exited")
        self.assertEqual(run.record["termination"]["leftovers_at_trigger"], 1)

    def test_f4_daemonizer(self):
        run = self.scenario("corral_daemonizer")
        self.assertEqual(run.code, 0)
        self.assertEqual(run.record["termination"]["leftovers_at_trigger"], 1)

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
        self.assertEqual(run.record["exit_reason"], "cancelled")
        self.assertEqual(run.record["cancel_signal"], signal.SIGINT)

    def test_sigterm_cancels_f5(self):
        run = self.scenario("corral_daemonizer_stay", options=["--grace", "1s"],
                            signal_after=0.5, sig=signal.SIGTERM)
        self.assertEqual(run.code, 143)


if __name__ == "__main__":
    unittest.main()
