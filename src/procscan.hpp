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

// Parses one /proc/<pid>/stat line. The command name may itself contain
// spaces and parentheses, so fields are read after the last ')'.
std::optional<ProcStat> parse_proc_stat(std::string_view line);

// nullopt if the process no longer exists.
std::optional<ProcStat> read_proc_stat(pid_t pid);

std::vector<pid_t> list_procs();

// 'X' (dead) is transient and treated like 'Z'.
bool is_live(const ProcStat& st);

// A process belongs to the run if its parent is `self`, or if its process
// group or session is the main child's.
struct FallbackTarget {
  pid_t self;
  pid_t child;
  bool matches(pid_t pid, const ProcStat& st) const;
};

std::vector<pid_t> fallback_live_set(const FallbackTarget& target);

// Signals `pid` only if it is still a live member of `target`.
void safe_signal(pid_t pid, int sig, const FallbackTarget& target);

}  // namespace corral
