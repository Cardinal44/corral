#pragma once

#include <sys/types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "clock.hpp"
#include "runner.hpp"

namespace corral {

// The value of `key` in a flat keyed file such as cgroup.events or
// memory.events, where each line is "<key> <number>".
std::optional<uint64_t> parse_keyed_value(std::string_view text, std::string_view key);

// A cgroup v2 directory that holds every process of one run.
class Box {
 public:
  explicit Box(std::string path) : path_(std::move(path)) {}

  const std::string& path() const { return path_; }
  // The path as /proc/<pid>/cgroup shows it.
  std::string relative_path() const;

  std::vector<pid_t> live_members() const;
  // True also when cgroup.events cannot be read, so that verification waits.
  bool populated() const;
  // SIGKILLs every process in the box and its descendants, atomically.
  void kill() const;
  LimitEvents events() const;
  // Retries while the kernel still reports EBUSY, until `deadline`.
  bool remove(TimePoint deadline, std::string& error) const;

 private:
  std::string path_;
};

struct BoxSetup {
  std::optional<Box> box;
  // Set when there is no box: the mode_reason value and the failed step.
  std::string reason;
  std::string error;
  std::optional<bool> mem_applied;
  std::optional<bool> pids_applied;
  std::vector<std::string> warnings;
};

// Creates the box under spec.cgroup_parent, or else under this process's own
// delegated cgroup, which first requires moving this process into a leaf.
BoxSetup create_box(const RunSpec& spec);

}  // namespace corral
