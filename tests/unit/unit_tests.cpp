// Exits nonzero if any CHECK fails.

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "audit.hpp"
#include "cgroup.hpp"
#include "cli.hpp"
#include "procscan.hpp"
#include "runner.hpp"
#include "unique_fd.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    ++g_checks;                                                            \
    if (!(cond)) {                                                         \
      ++g_failures;                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #cond);                                                 \
    }                                                                      \
  } while (0)

using namespace std::chrono_literals;
using corral::ExitReason;

void test_parse_duration() {
  using corral::parse_duration;
  CHECK(parse_duration("500ms") == 500ms);
  CHECK(parse_duration("2s") == 2000ms);
  CHECK(parse_duration("1m") == 60000ms);
  CHECK(parse_duration("1h") == 3600000ms);
  CHECK(parse_duration("3") == 3000ms);
  CHECK(parse_duration("0") == 0ms);
  CHECK(!parse_duration(""));
  CHECK(!parse_duration("5x"));
  CHECK(!parse_duration("-1s"));
  CHECK(!parse_duration("s"));
  CHECK(!parse_duration("1.5s"));
  CHECK(!parse_duration("99999999999999999999"));   // overflows uint64
  CHECK(!parse_duration("9223372036854775807s"));   // overflows int64 ms
  CHECK(parse_duration("9223372036854775807ms") == std::chrono::milliseconds::max());
}

void test_parse_size() {
  using corral::parse_size;
  CHECK(parse_size("100") == 100u);
  CHECK(parse_size("4K") == 4096u);
  CHECK(parse_size("64M") == 64u * 1024 * 1024);
  CHECK(parse_size("1G") == 1024u * 1024 * 1024);
  CHECK(!parse_size("0"));
  CHECK(!parse_size("0K"));
  CHECK(!parse_size("1T"));
  CHECK(!parse_size(""));
  CHECK(!parse_size("4k"));
  CHECK(!parse_size("-4K"));
  CHECK(!parse_size("99999999999999999999"));
  CHECK(!parse_size("17179869184G"));  // 2^34 GiB = 2^64 bytes
}

void test_parse_count() {
  using corral::parse_count;
  CHECK(parse_count("32") == 32u);
  CHECK(!parse_count("0"));
  CHECK(!parse_count("32K"));
  CHECK(!parse_count(""));
}

void test_parse_args() {
  using corral::parse_args;
  {
    auto p = parse_args({"--wall", "5s", "--grace=0", "--", "echo", "hi"});
    CHECK(p.options.has_value());
    CHECK(p.options->spec.wall == 5000ms);
    CHECK(p.options->spec.grace == 0ms);
    CHECK((p.options->spec.argv == std::vector<std::string>{"echo", "hi"}));
  }
  {
    // Parsing stops at the first non-option; later flags belong to the command.
    auto p = parse_args({"--quiet", "echo", "--wall", "5s"});
    CHECK(p.options.has_value());
    CHECK(p.options->quiet);
    CHECK(!p.options->spec.wall);
    CHECK((p.options->spec.argv == std::vector<std::string>{"echo", "--wall", "5s"}));
  }
  {
    auto p = parse_args({"--mem", "64M", "--pids", "32", "--max-output", "1000",
                         "--cgroup-mode=fallback", "--cgroup-parent", "/sys/fs/cgroup/x",
                         "--stdin", "inherit", "--json", "/dev/fd/3", "--verify-timeout",
                         "500ms", "true"});
    CHECK(p.options.has_value());
    const corral::RunSpec& s = p.options->spec;
    CHECK(s.mem_bytes == 64u * 1024 * 1024);
    CHECK(s.pids == 32u);
    CHECK(s.max_output_bytes == 1000u);
    CHECK(s.mode == corral::ModeRequest::Fallback);
    CHECK(s.cgroup_parent == std::string("/sys/fs/cgroup/x"));
    CHECK(s.stdin_policy == corral::StdinPolicy::Inherit);
    CHECK(p.options->json_path == std::string("/dev/fd/3"));
    CHECK(s.verify_timeout == 500ms);
  }
  {
    auto p = parse_args({"--help"});
    CHECK(p.options.has_value() && p.options->help);
  }
  CHECK(!parse_args({}).options);
  CHECK(!parse_args({"--wall", "5x", "true"}).options);
  CHECK(!parse_args({"--wall"}).options);
  CHECK(!parse_args({"--bogus", "true"}).options);
  CHECK(!parse_args({"-q", "true"}).options);
  CHECK(!parse_args({"--quiet=1", "true"}).options);
  CHECK(!parse_args({"--cgroup-mode", "x", "true"}).options);
  CHECK(!parse_args({"--stdin", "tty", "true"}).options);
  CHECK(!parse_args({"--pids", "0", "true"}).options);
}

void test_parse_proc_stat() {
  using corral::parse_proc_stat;
  auto st = parse_proc_stat("1234 (bash) S 1000 1234 1234 34816 1234 4194560 0 0");
  CHECK(st.has_value());
  CHECK(st->comm == "bash");
  CHECK(st->state == 'S');
  CHECK(st->ppid == 1000);
  CHECK(st->pgrp == 1234);
  CHECK(st->session == 1234);

  st = parse_proc_stat("77 (a) (b c) Z 1 2 3 0 -1 0");
  CHECK(st.has_value());
  CHECK(st->comm == "a) (b c");
  CHECK(st->state == 'Z');
  CHECK(st->ppid == 1);
  CHECK(st->pgrp == 2);
  CHECK(st->session == 3);

  CHECK(!parse_proc_stat(""));
  CHECK(!parse_proc_stat("12 bash S 1 2 3"));
  CHECK(!parse_proc_stat("12 (bash) S 1 2"));
  CHECK(!parse_proc_stat("12 (bash) S 1 x 3"));
}

void test_parse_proc_cgroup() {
  using corral::parse_proc_cgroup;
  CHECK(parse_proc_cgroup("0::/user.slice/run-1.scope\n") ==
        std::string("/user.slice/run-1.scope"));
  CHECK(parse_proc_cgroup("0::/\n") == std::string("/"));
  // A hybrid hierarchy lists v1 controllers first.
  CHECK(parse_proc_cgroup("12:pids:/x\n1:name=systemd:/y\n0::/z") == std::string("/z"));
  CHECK(!parse_proc_cgroup("12:pids:/x\n"));
  CHECK(!parse_proc_cgroup(""));
}

void test_parse_keyed_value() {
  using corral::parse_keyed_value;
  CHECK(parse_keyed_value("populated 1\nfrozen 0\n", "populated") == 1u);
  CHECK(parse_keyed_value("populated 0\nfrozen 0\n", "populated") == 0u);
  const char* memory_events = "low 0\nhigh 0\nmax 12\noom 1\noom_kill 3\noom_group_kill 1\n";
  CHECK(parse_keyed_value(memory_events, "oom_kill") == 3u);
  CHECK(parse_keyed_value(memory_events, "max") == 12u);
  CHECK(parse_keyed_value("max 0\nmax.imposed 4\n", "max") == 0u);  // exact key only
  CHECK(!parse_keyed_value(memory_events, "oom_ki"));
  CHECK(!parse_keyed_value("populated x\n", "populated"));
  CHECK(!parse_keyed_value("", "populated"));
}

void test_json_quote() {
  using corral::json_quote;
  CHECK(json_quote("") == "\"\"");
  CHECK(json_quote("plain") == "\"plain\"");
  CHECK(json_quote("a\"b") == "\"a\\\"b\"");
  CHECK(json_quote("a\\b") == "\"a\\\\b\"");
  CHECK(json_quote("a\nb\tc\rd\be\ff") == "\"a\\nb\\tc\\rd\\be\\ff\"");
  CHECK(json_quote("\x01") == "\"\\u0001\"");
  CHECK(json_quote("\x1f") == "\"\\u001f\"");
  CHECK(json_quote(std::string("a\0b", 3)) == "\"a\\u0000b\"");
  CHECK(json_quote("\x7f") == "\"\x7f\"");
  CHECK(json_quote("caf\xc3\xa9 \xe2\x9c\x93") == "\"caf\xc3\xa9 \xe2\x9c\x93\"");  // UTF-8
  CHECK(json_quote("\xff") == "\"\xff\"");  // not UTF-8, still passes through
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

void test_audit_json() {
  corral::RunResult r;
  r.corral_version = "0.1.0";
  r.argv = {"sh", "-c", "echo \"hi\""};
  r.mode_reason = "forced_by_flag";
  r.limits.wall_ms = 2000;
  r.limits.grace_ms = 1000;
  r.limits.verify_timeout_ms = 2000;
  r.limits_enforced.wall = true;
  r.exit_reason = ExitReason::Timeout;
  r.term_signal = SIGTERM;
  r.corral_exit_code = 124;
  r.termination.term_sent_ms = 2001;
  r.survivors = {41, 42};
  r.kernel = "6.8.0";
  const std::string json = corral::audit_json(r);

  CHECK(json.rfind("{\"schema_version\": 1, \"corral_version\": \"0.1.0\", ", 0) == 0);
  CHECK(json.size() >= 2 && json.substr(json.size() - 2) == "}\n");
  CHECK(json.find('\n') == json.size() - 1);  // one line
  CHECK(contains(json, "\"argv\": [\"sh\", \"-c\", \"echo \\\"hi\\\"\"]"));
  CHECK(contains(json, "\"mode\": \"fallback\", \"mode_reason\": \"forced_by_flag\""));
  CHECK(contains(json, "\"limits\": {\"wall_ms\": 2000, \"grace_ms\": 1000, "
                       "\"verify_timeout_ms\": 2000, \"mem_bytes\": null, \"pids\": null, "
                       "\"output_bytes\": null}"));
  CHECK(contains(json, "\"limits_enforced\": {\"wall\": true, \"mem\": null, \"pids\": null, "
                       "\"output\": null}"));
  CHECK(contains(json, "\"exit_reason\": \"timeout\", \"exit_code\": null, \"term_signal\": 15, "
                       "\"cancel_signal\": null, \"spawn_errno\": null, \"corral_exit_code\": 124"));
  CHECK(contains(json, "\"truncated\": false"));
  CHECK(contains(json, "\"term_sent_ms\": 2001, \"kill_sent_ms\": null"));
  CHECK(contains(json, "\"survivors\": [41, 42]"));
  CHECK(contains(json, "\"limit_events\": {\"oom_kills\": null, \"pids_max_hits\": null, "
                       "\"peak_mem_bytes\": null}, \"cgroup_path\": null, \"kernel\": \"6.8.0\"}"));

  r.survivors.clear();
  CHECK(contains(corral::audit_json(r), "\"survivors\": []"));
}

void test_summary_line() {
  corral::RunResult r;
  r.exit_reason = ExitReason::Timeout;
  r.corral_exit_code = 124;
  r.duration_ms = 3014;
  r.termination.leftovers_at_trigger = 1;
  CHECK(corral::summary_line(r) ==
        "corral: timeout code=124 in 3.014s mode=fallback leftovers_killed=1\n");
  r.exit_reason = ExitReason::Exited;
  r.corral_exit_code = 0;
  r.duration_ms = 5;
  r.termination.leftovers_at_trigger = 0;
  CHECK(corral::summary_line(r) ==
        "corral: exited code=0 in 0.005s mode=fallback leftovers_killed=0\n");
}

corral::RunResult result_with(ExitReason reason) {
  corral::RunResult r;
  r.exit_reason = reason;
  return r;
}

void test_exit_code_for() {
  using corral::exit_code_for;
  auto r = result_with(ExitReason::Exited);
  r.exit_code = 0;
  CHECK(exit_code_for(r) == 0);
  r.exit_code = 7;
  CHECK(exit_code_for(r) == 7);

  r = result_with(ExitReason::Signaled);
  r.term_signal = SIGKILL;
  CHECK(exit_code_for(r) == 137);

  CHECK(exit_code_for(result_with(ExitReason::Timeout)) == 124);
  CHECK(exit_code_for(result_with(ExitReason::MemoryLimit)) == 121);
  CHECK(exit_code_for(result_with(ExitReason::OutputLimit)) == 122);

  r = result_with(ExitReason::Cancelled);
  r.cancel_signal = SIGINT;
  CHECK(exit_code_for(r) == 130);
  r.cancel_signal = SIGTERM;
  CHECK(exit_code_for(r) == 143);
  r.cancel_signal = SIGHUP;
  CHECK(exit_code_for(r) == 129);

  r = result_with(ExitReason::SpawnFailed);
  r.spawn_errno = ENOENT;
  CHECK(exit_code_for(r) == 127);
  r.spawn_errno = EACCES;
  CHECK(exit_code_for(r) == 126);

  CHECK(exit_code_for(result_with(ExitReason::SetupFailed)) == 125);

  // 120 overrides everything.
  for (ExitReason reason : {ExitReason::Exited, ExitReason::Signaled, ExitReason::Timeout,
                            ExitReason::MemoryLimit, ExitReason::OutputLimit,
                            ExitReason::Cancelled, ExitReason::SpawnFailed}) {
    r = result_with(reason);
    r.exit_code = 0;
    r.survivors = {12345};
    CHECK(exit_code_for(r) == 120);
    r.survivors.clear();
    r.verify_failed = true;  // for example, /proc could not be read
    CHECK(exit_code_for(r) == 120);
  }
}

corral::RunSpec capture_spec(std::vector<std::string> argv) {
  corral::RunSpec spec;
  spec.argv = std::move(argv);
  spec.mode = corral::ModeRequest::Fallback;
  spec.output_mode = corral::OutputMode::Capture;
  return spec;
}

int count_open_fds() {
  DIR* d = opendir("/proc/self/fd");
  if (d == nullptr) return -1;
  int n = 0;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] != '.') ++n;
  }
  closedir(d);
  return n;
}

void test_descriptor_hygiene() {
  // run() leaves the caller's descriptor table unchanged.
  const int before = count_open_fds();
  CHECK(before > 0);
  for (int i = 0; i < 100; ++i) {
    corral::RunResult r = corral::run(capture_spec({"/bin/true"}));
    CHECK(r.exit_reason == ExitReason::Exited);
  }
  CHECK(count_open_fds() == before);

  // The command receives only fds 0, 1, 2, even ones the host leaked
  // without O_CLOEXEC (deliberately, here).
  corral::unique_fd leaked(open("/dev/null", O_RDONLY));
  CHECK(leaked.get() >= 3);
  const std::string probe = "[ -e /proc/$$/fd/" + std::to_string(leaked.get()) + " ]";
  corral::RunResult r = corral::run(capture_spec({"sh", "-c", probe}));
  CHECK(r.exit_reason == ExitReason::Exited);
  CHECK(r.exit_code == 1);
  r = corral::run(capture_spec({"sh", "-c", "[ -e /proc/$$/fd/2 ]"}));
  CHECK(r.exit_code == 0);
}

void test_transparency() {
  corral::RunResult r = corral::run(capture_spec({"sh", "-c", "exit 7"}));
  CHECK(r.exit_reason == ExitReason::Exited);
  CHECK(r.exit_code == 7);
  CHECK(r.corral_exit_code == 7);

  r = corral::run(capture_spec({"sh", "-c", "kill -9 $$"}));
  CHECK(r.exit_reason == ExitReason::Signaled);
  CHECK(r.term_signal == SIGKILL);
  CHECK(r.corral_exit_code == 137);
}

void test_capture_and_stdin() {
  corral::RunResult r = corral::run(capture_spec({"sh", "-c", "echo out; echo err >&2"}));
  CHECK(r.stdout_data == "out\n");
  CHECK(r.stderr_data == "err\n");
  CHECK(r.output.stdout_bytes == 4);
  CHECK(r.output.stderr_bytes == 4);

  r = corral::run(capture_spec({"cat"}));
  CHECK(r.exit_reason == ExitReason::Exited);
  CHECK(r.exit_code == 0);
  CHECK(r.stdout_data.empty());
}

void test_timeout() {
  corral::RunSpec spec = capture_spec({"sleep", "30"});
  spec.wall = 200ms;
  corral::RunResult r = corral::run(spec);
  CHECK(r.exit_reason == ExitReason::Timeout);
  CHECK(r.corral_exit_code == 124);
  CHECK(r.term_signal == SIGTERM);
  CHECK(r.termination.term_sent_ms.has_value());
  CHECK(!r.termination.kill_sent_ms.has_value());
  CHECK(r.limits_enforced.wall == true);
  CHECK(r.duration_ms < 2000);
  CHECK(r.error.empty());
}

void test_leftovers_are_killed() {
  // The background sleep outlives sh and must be terminated and reaped.
  corral::RunResult r = corral::run(capture_spec({"sh", "-c", "sleep 30 & exit 3"}));
  CHECK(r.exit_reason == ExitReason::Exited);
  CHECK(r.corral_exit_code == 3);
  CHECK(r.termination.leftovers_at_trigger == 1);
  CHECK(r.orphans_reaped == 1);
  CHECK(r.survivors.empty());
  CHECK(r.duration_ms < 2000);
  CHECK(r.error.empty());
}

void test_output_cap() {
  // The cap is on stdout plus stderr together, cut at the exact byte.
  corral::RunSpec spec = capture_spec({"sh", "-c", "printf ab; printf cd >&2; sleep 30"});
  spec.max_output_bytes = 3;
  corral::RunResult r = corral::run(spec);
  CHECK(r.exit_reason == ExitReason::OutputLimit);
  CHECK(r.corral_exit_code == 122);
  CHECK(r.stdout_data == "ab");
  CHECK(r.stderr_data == "c");
  CHECK(r.output.stdout_bytes == 2);
  CHECK(r.output.stderr_bytes == 1);
  CHECK(r.output.truncated);
  CHECK(r.limits_enforced.output == true);
  CHECK(r.limits.output_bytes == 3u);
  CHECK(r.term_signal == SIGTERM);  // status captured after the cap trigger
  CHECK(r.duration_ms < 2000);
  CHECK(r.error.empty());

  // Output that fits the cap exactly is not truncated.
  spec = capture_spec({"sh", "-c", "printf abc"});
  spec.max_output_bytes = 3;
  r = corral::run(spec);
  CHECK(r.exit_reason == ExitReason::Exited);
  CHECK(r.corral_exit_code == 0);
  CHECK(r.stdout_data == "abc");
  CHECK(!r.output.truncated);

  // The main child overran the cap and then exited: the cap came first.
  spec = capture_spec({"sh", "-c", "printf abcdef"});
  spec.max_output_bytes = 3;
  r = corral::run(spec);
  CHECK(r.exit_reason == ExitReason::OutputLimit);
  CHECK(r.stdout_data == "abc");
  CHECK(r.error.empty());

  // The command exits by itself, then a leftover's SIGTERM handler writes past
  // the cap during termination. Its stderr is discarded, so only stdout races
  // for the remaining byte.
  spec = capture_spec({"sh", "-c",
                       "printf ab; (trap 'printf cdef; exit' TERM; "
                       "while :; do sleep 0.1; done) 2>/dev/null & exit 0"});
  spec.max_output_bytes = 3;
  r = corral::run(spec);
  CHECK(r.exit_reason == ExitReason::OutputLimit);
  CHECK(r.corral_exit_code == 122);
  CHECK(r.exit_code == 0);  // the command's own status is kept
  CHECK(r.stdout_data == "abc");
  CHECK(r.output.truncated);
  CHECK(r.error.empty());

  // No cap requested: nothing reported as enforced.
  r = corral::run(capture_spec({"true"}));
  CHECK(!r.limits_enforced.output.has_value());
}

void test_spawn_failure() {
  corral::RunResult r = corral::run(capture_spec({"/nonexistent"}));
  CHECK(r.exit_reason == ExitReason::SpawnFailed);
  CHECK(r.spawn_errno == ENOENT);
  CHECK(r.corral_exit_code == 127);

  r = corral::run(capture_spec({"/dev/null"}));  // exists, not executable
  CHECK(r.exit_reason == ExitReason::SpawnFailed);
  CHECK(r.spawn_errno == EACCES);
  CHECK(r.corral_exit_code == 126);

  r = corral::run(capture_spec({}));
  CHECK(r.exit_reason == ExitReason::SetupFailed);
  CHECK(r.corral_exit_code == 125);
}

}  // namespace

int main() {
  test_parse_duration();
  test_parse_size();
  test_parse_count();
  test_parse_args();
  test_parse_proc_stat();
  test_parse_proc_cgroup();
  test_parse_keyed_value();
  test_json_quote();
  test_audit_json();
  test_summary_line();
  test_exit_code_for();
  test_descriptor_hygiene();
  test_transparency();
  test_capture_and_stdin();
  test_timeout();
  test_leftovers_are_killed();
  test_output_cap();
  test_spawn_failure();
  std::printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
