#pragma once

#include <sys/types.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace corral {

struct ProcStat {
  std::string comm;
  char state = '?';
  pid_t ppid = 0;
  pid_t pgrp = 0;
  pid_t session = 0;
};

// The whole file; nullopt if it cannot be opened or read.
std::optional<std::string> read_file(const std::string& path);

// Parses one /proc/<pid>/stat line. The command name may itself contain
// spaces and parentheses, so fields are read after the last ')'.
std::optional<ProcStat> parse_proc_stat(std::string_view line);

// nullopt if the process no longer exists.
std::optional<ProcStat> read_proc_stat(pid_t pid);

// The cgroup v2 path from the "0::" line of a /proc/<pid>/cgroup file.
std::optional<std::string> parse_proc_cgroup(std::string_view file);

// nullopt if the process no longer exists.
std::optional<std::string> read_proc_cgroup(pid_t pid);

// nullopt with errno set if /proc cannot be listed.
std::optional<std::vector<pid_t>> list_procs();

// 'X' (dead) is transient and treated like 'Z'.
bool is_live(const ProcStat& st);

struct RunTarget {
  pid_t self;
  pid_t child;
  std::string box;  // the box's path as /proc/<pid>/cgroup shows it; empty without a box

  // Parent is `self`, or process group or session is the main child's.
  bool in_fallback_set(pid_t pid, const ProcStat& st) const;
  // Live, and in the fallback set or in the box.
  bool matches(pid_t pid, const ProcStat& st) const;
};

struct LiveSet {
  std::vector<pid_t> pids;
  // Set when a source could not be read. The set is then incomplete, and an
  // empty `pids` proves nothing.
  std::string error;

  bool empty() const { return error.empty() && pids.empty(); }
};

LiveSet fallback_live_set(const RunTarget& target);

// Signals `pid` only if it is still a live member of `target`.
void safe_signal(pid_t pid, int sig, const RunTarget& target);

}  // namespace corral
