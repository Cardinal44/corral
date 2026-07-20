#pragma once

#include <sys/types.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace corral {

enum class Mode { Enforced, Fallback };
enum class ModeRequest { Auto, Enforced, Fallback };
enum class StdinPolicy { Null, Inherit };
enum class OutputMode { Forward, Capture, Discard };
enum class ExitReason { Exited, Signaled, Timeout, MemoryLimit,
                        OutputLimit, Cancelled, SpawnFailed, SetupFailed };

struct RunSpec {
  std::vector<std::string> argv;                  // non-empty
  std::optional<std::chrono::milliseconds> wall;
  std::chrono::milliseconds grace{2000};
  std::chrono::milliseconds verify_timeout{2000};
  std::optional<uint64_t> mem_bytes;
  std::optional<uint64_t> pids;
  std::optional<uint64_t> max_output_bytes;
  ModeRequest mode = ModeRequest::Auto;
  std::optional<std::string> cgroup_parent;
  StdinPolicy stdin_policy = StdinPolicy::Null;
  OutputMode output_mode = OutputMode::Forward;
  int cancel_fd = -1;   // if >= 0 and becomes readable, the run is cancelled.
                        // run() never reads from it.
};

// nullopt means not requested.
struct Limits {
  std::optional<int64_t> wall_ms;
  int64_t grace_ms = 0;
  int64_t verify_timeout_ms = 0;
  std::optional<uint64_t> mem_bytes;
  std::optional<uint64_t> pids;
  std::optional<uint64_t> output_bytes;
};

// true: applied. false: requested but not applied. nullopt: not requested.
struct LimitsEnforced {
  std::optional<bool> wall, mem, pids, output;
};

struct OutputStats {
  uint64_t stdout_bytes = 0;
  uint64_t stderr_bytes = 0;
  bool truncated = false;
};

struct Termination {
  int64_t trigger_ms = 0;
  uint64_t leftovers_at_trigger = 0;
  std::optional<int64_t> term_sent_ms;
  std::optional<int64_t> kill_sent_ms;
  int64_t verified_ms = 0;
};

struct LimitEvents {
  std::optional<uint64_t> oom_kills, pids_max_hits, peak_mem_bytes;
};

struct RunResult {
  // Audit record fields.
  int schema_version = 1;
  std::string corral_version;
  std::vector<std::string> argv;
  Mode mode = Mode::Fallback;
  std::optional<std::string> mode_reason;
  Limits limits;
  LimitsEnforced limits_enforced;
  std::string started_at;
  int64_t duration_ms = 0;
  ExitReason exit_reason = ExitReason::SetupFailed;
  std::optional<int> exit_code;
  std::optional<int> term_signal;
  std::optional<int> cancel_signal;
  std::optional<int> spawn_errno;
  int corral_exit_code = 125;
  OutputStats output;
  Termination termination;
  uint64_t orphans_reaped = 0;
  uint64_t orphans_remaining = 0;
  std::vector<pid_t> survivors;
  LimitEvents limit_events;
  std::optional<std::string> cgroup_path;
  std::string kernel;

  // Not part of the audit record.
  std::string stdout_data, stderr_data;  // filled only in OutputMode::Capture
  bool cancelled = false;
  std::string error;  // for setup_failed and spawn_failed; run() never prints
  std::vector<std::string> warnings;  // for example a requested limit that was not applied
  bool teardown_failed = false;       // the box could not be removed; exit 120
};

RunResult run(const RunSpec& spec);
int exit_code_for(const RunResult& r);

}  // namespace corral
