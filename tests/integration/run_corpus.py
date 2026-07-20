"""Integration tests against the real corral binary.

Every scenario runs in both modes with a fresh TAG and `--json`, and must leave
zero surviving processes. Enforced mode needs a delegated cgroup, so run the
suite through scripts/run-integration.sh. CORRAL_RACE=1 repeats every scenario
20 times.
"""

import json
import os
import signal
import tempfile
import unittest

import harness

BUILD = os.environ.get(
    "CORRAL_BUILD_DIR", os.path.join(os.path.dirname(__file__), "..", "..", "build"))
CORRAL = os.path.join(BUILD, "corral")
FAULTS = os.path.join(BUILD, "faults")
STANDARD = ["--wall", "2s", "--grace", "1s", "--verify-timeout", "2s"]
REPEAT = 20 if os.environ.get("CORRAL_RACE") == "1" else 1

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


def run_corral(args, tag, **kwargs):
    with tempfile.TemporaryDirectory() as tmp:
        json_path = os.path.join(tmp, "record.json")
        run = harness.run([CORRAL, "--quiet", "--json", json_path, *args], tag, **kwargs)
        if os.path.exists(json_path):
            with open(json_path, "rb") as f:
                run.record = f.read()
    return run


def parse_record(test, run, argv):
    """Checks the record's shape and the fields every run shares."""
    raw = run.record
    test.assertIsNotNone(raw, "no record written")
    test.assertTrue(raw.endswith(b"}\n"), raw)
    test.assertEqual(raw.count(b"\n"), 1, "the record is one line")
    record = json.loads(raw)
    test.assertEqual(list(record), RECORD_FIELDS)
    for name, fields in NESTED_FIELDS.items():
        test.assertEqual(list(record[name]), fields, name)
    test.assertEqual(record["schema_version"], 1)
    test.assertEqual(record["argv"], argv)
    test.assertEqual(record["corral_exit_code"], run.code)
    test.assertEqual(record["kernel"], os.uname().release)
    return record


class Corpus:
    """The scenarios. Subclasses set MODE and mode_options."""

    MODE = None
    mode_options = []

    @property
    def enforced(self):
        return self.MODE == "enforced"

    def scenario(self, program, *program_args, check, options=STANDARD, stderr=b"",
                 **kwargs):
        """Runs `program` under corral REPEAT times and calls check(run) on each."""
        for rep in range(REPEAT):
            with self.subTest(rep=rep):
                tag = harness.new_tag()
                argv = [os.path.join(FAULTS, program), tag, *program_args]
                try:
                    run = run_corral([*self.mode_options, *options, "--", *argv], tag,
                                     **kwargs)
                finally:
                    leaked = harness.kill_tagged(tag)
                self.assertFalse(run.hung, "corral did not return")
                self.assertEqual(run.survivors, [], f"stderr: {run.stderr!r}")
                self.assertEqual(leaked, [], "cleanup found tagged processes")
                # The fault programs write nothing to stderr, so anything there
                # is a corral diagnostic.
                self.assertEqual(run.stderr, stderr)
                run.record = self.check_record(run, argv)
                check(run)

    def check_record(self, run, argv):
        record = parse_record(self, run, argv)
        self.assertEqual(record["mode"], self.MODE)
        self.assertEqual(record["orphans_remaining"], 0)
        self.assertEqual(record["survivors"], [])
        events = record["limit_events"]
        if self.enforced:
            self.assertIsNone(record["mode_reason"])
            box = record["cgroup_path"]
            self.assertTrue(box.startswith(self.runs + "/corral-run-"), box)
            self.assertFalse(os.path.exists(box), "the box was not removed")
            self.assertIsInstance(events["oom_kills"], int)
            self.assertIsInstance(events["pids_max_hits"], int)
        else:
            self.assertEqual(record["mode_reason"], "forced_by_flag")
            self.assertIsNone(record["cgroup_path"])
            self.assertEqual(events,
                             {"oom_kills": None, "pids_max_hits": None, "peak_mem_bytes": None})
        limits = record["limits"]
        if limits["wall_ms"] is not None:
            # corral must return within wall + grace + verify_timeout + 500 ms.
            bound_ms = limits["wall_ms"] + limits["grace_ms"] + limits["verify_timeout_ms"] + 500
            self.assertLessEqual(run.elapsed * 1000, bound_ms)
        return record

    # The fault corpus with the standard parameters.

    def test_f1_streamer(self):
        def check(run):
            self.assertEqual(run.code, 124)
            self.assertGreater(len(run.stdout), 0)
            self.assertEqual(run.record["output"]["stdout_bytes"], len(run.stdout))
            self.assertFalse(run.record["output"]["truncated"])
        self.scenario("corral_streamer", check=check)

    def test_f2_stdin_waiter(self):
        def check(run):
            self.assertEqual(run.code, 0)
            self.assertEqual(run.record["exit_reason"], "exited")
            self.assertLess(run.elapsed, 0.5)
        self.scenario("corral_stdin_waiter", check=check)

    def test_f3_script_background_job(self):
        def check(run):
            self.assertEqual(run.code, 0)
            self.assertEqual(run.record["exit_reason"], "exited")
            self.assertEqual(run.record["termination"]["leftovers_at_trigger"], 1)
        self.scenario("corral_script_spawner.sh", check=check)

    def test_f4_daemonizer(self):
        def check(run):
            self.assertEqual(run.code, 0)
            self.assertEqual(run.record["exit_reason"], "exited")
            self.assertEqual(run.record["termination"]["leftovers_at_trigger"], 1)
        self.scenario("corral_daemonizer", check=check)

    def test_f5_daemonizer_stay(self):
        def check(run):
            self.assertEqual(run.code, 124)
            self.assertEqual(run.record["exit_reason"], "timeout")
        self.scenario("corral_daemonizer_stay", check=check)

    def test_f6_term_ignorer(self):
        def check(run):
            self.assertEqual(run.code, 124)
            self.assertGreaterEqual(run.elapsed, 3.0)  # wall 2 s + grace 1 s
            self.assertEqual(run.record["term_signal"], signal.SIGKILL)
            self.assertIsNotNone(run.record["termination"]["kill_sent_ms"])
        self.scenario("corral_term_ignorer", check=check)

    def test_f7_forker(self):
        def check(run):
            self.assertEqual(run.code, 124)
            self.assertGreater(run.record["termination"]["leftovers_at_trigger"], 0)
            if self.enforced:
                self.assertIs(run.record["limits_enforced"]["pids"], True)
                self.assertGreater(run.record["limit_events"]["pids_max_hits"], 0)
        options = STANDARD + (["--pids", "32"] if self.enforced else [])
        self.scenario("corral_forker", options=options, check=check)

    def test_f8_pipe_holder(self):
        def check(run):
            self.assertEqual(run.code, 0)
            self.assertEqual(run.record["exit_reason"], "exited")
            self.assertEqual(run.stdout, b"hello\n")
            self.assertLess(run.elapsed, 1.5)
        self.scenario("corral_pipe_holder", check=check)

    def test_f9_mem_hog(self):
        def check(run):
            if self.enforced:
                self.assertEqual(run.code, 121)
                self.assertEqual(run.record["exit_reason"], "memory_limit")
                self.assertIs(run.record["limits_enforced"]["mem"], True)
                self.assertGreater(run.record["limit_events"]["oom_kills"], 0)
            else:
                self.assertEqual(run.code, 124)
                self.assertIsNone(run.record["limits_enforced"]["mem"])
        options = STANDARD + (["--mem", "64M"] if self.enforced else [])
        self.scenario("corral_mem_hog", "256", options=options, check=check)

    def test_f10_graceful(self):
        def check(run):
            self.assertEqual(run.code, 124)
            self.assertEqual(run.stdout, b"cleaning up\n")
            self.assertEqual(run.record["exit_code"], 0)
            self.assertIsNone(run.record["term_signal"])
            self.assertIsNotNone(run.record["termination"]["term_sent_ms"])
            self.assertIsNone(run.record["termination"]["kill_sent_ms"])
        self.scenario("corral_graceful", check=check)

    # Other checks.

    def test_wall_timeout(self):
        def check(run):
            self.assertEqual(run.code, 124)
            self.assertGreaterEqual(run.elapsed, 1.0)
            self.assertLessEqual(run.elapsed, 1.5)
            self.assertEqual(run.record["term_signal"], signal.SIGTERM)
            self.assertEqual(run.record["limits"]["wall_ms"], 1000)
            self.assertEqual(run.record["limits_enforced"]["wall"], True)
            self.assertIsNone(run.record["termination"]["kill_sent_ms"])
        self.scenario("corral_sleeper", "100", options=["--wall", "1s"], check=check)

    def test_f1_max_output(self):
        def check(run):
            self.assertEqual(run.code, 122)
            self.assertEqual(len(run.stdout), 1000)
            self.assertEqual(run.record["exit_reason"], "output_limit")
            self.assertEqual(run.record["output"],
                             {"stdout_bytes": 1000, "stderr_bytes": 0, "truncated": True})
            self.assertEqual(run.record["limits_enforced"]["output"], True)
        self.scenario("corral_streamer", options=["--max-output", "1000"], check=check)

    def test_mem_and_pids_reporting(self):
        # Applied only with a box; never reported as enforced without one.
        def check(run):
            self.assertEqual(run.code, 0)
            self.assertEqual(run.record["limits"]["mem_bytes"], 64 << 20)
            self.assertEqual(run.record["limits"]["pids"], 32)
            self.assertIs(run.record["limits_enforced"]["mem"], self.enforced)
            self.assertIs(run.record["limits_enforced"]["pids"], self.enforced)
        self.scenario("corral_sleeper", "0", options=["--mem", "64M", "--pids", "32"],
                      check=check)

    def test_nonexistent_command_exits_127(self):
        path = os.path.join(FAULTS, "corral_does_not_exist")
        message = f"corral: cannot execute '{path}': No such file or directory\n"

        def check(run):
            self.assertEqual(run.code, 127)
            self.assertEqual(run.record["exit_reason"], "spawn_failed")
            self.assertEqual(run.record["spawn_errno"], 2)
        self.scenario("corral_does_not_exist", stderr=message.encode(), check=check)

    def test_sigint_cancels_f5(self):
        def check(run):
            self.assertEqual(run.code, 130)
            self.assertEqual(run.record["exit_reason"], "cancelled")
            self.assertEqual(run.record["cancel_signal"], signal.SIGINT)
        self.scenario("corral_daemonizer_stay", options=["--grace", "1s"], signal_after=0.5,
                      check=check)

    def test_sigterm_cancels_f5(self):
        def check(run):
            self.assertEqual(run.code, 143)
            self.assertEqual(run.record["cancel_signal"], signal.SIGTERM)
        self.scenario("corral_daemonizer_stay", options=["--grace", "1s"], signal_after=0.5,
                      sig=signal.SIGTERM, check=check)


class FallbackCorpus(Corpus, unittest.TestCase):
    MODE = "fallback"
    mode_options = ["--cgroup-mode=fallback"]


class EnforcedCorpus(Corpus, unittest.TestCase):
    MODE = "enforced"

    @classmethod
    def setUpClass(cls):
        cls.runs = harness.enforced_parent()
        if cls.runs is None:
            raise unittest.SkipTest(
                "no delegated cgroup; run the suite through scripts/run-integration.sh")
        cls.mode_options = ["--cgroup-mode=enforced", "--cgroup-parent", cls.runs]


class OutsideScope(unittest.TestCase):
    """Checks that need a cgroup that is not delegated to this user."""

    @classmethod
    def setUpClass(cls):
        g = harness.own_cgroup()
        if g is None or os.access(g, os.W_OK):
            raise unittest.SkipTest("this process's cgroup is delegated")

    def run_sleeper(self, *options):
        tag = harness.new_tag()
        argv = [os.path.join(FAULTS, "corral_sleeper"), tag, "0"]
        try:
            run = run_corral([*options, "--", *argv], tag)
        finally:
            leaked = harness.kill_tagged(tag)
        self.assertEqual(run.survivors, [])
        self.assertEqual(leaked, [])
        return run, parse_record(self, run, argv)

    def test_enforced_exits_125(self):
        run, record = self.run_sleeper("--cgroup-mode=enforced")
        self.assertEqual(run.code, 125)
        self.assertEqual(record["exit_reason"], "setup_failed")
        self.assertEqual(record["mode_reason"], "cgroup_not_delegated")
        self.assertIn(b"enforced mode unavailable (cgroup_not_delegated): access ", run.stderr)

    def test_auto_falls_back(self):
        run, record = self.run_sleeper("--mem", "64M")
        self.assertEqual(run.code, 0)
        self.assertEqual(record["mode"], "fallback")
        self.assertEqual(record["mode_reason"], "cgroup_not_delegated")
        self.assertIs(record["limits_enforced"]["mem"], False)
        self.assertIn(b"warning: cgroup_not_delegated", run.stderr)


if __name__ == "__main__":
    unittest.main()
