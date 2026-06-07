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

#include "cli.hpp"
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

  r =corral::run(capture_spec({"cat"}));
  CHECK(r.exit_reason == ExitReason::Exited);
  CHECK(r.exit_code == 0);
  CHECK(r.stdout_data.empty());
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
  test_exit_code_for();
  test_descriptor_hygiene();
  test_transparency();
  test_capture_and_stdin();
  test_spawn_failure();
  std::printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
