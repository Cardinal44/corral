#pragma once

#include <sys/types.h>

#include <optional>
#include <string>
#include <vector>

#include "runner.hpp"
#include "unique_fd.hpp"

namespace corral {

struct Spawned {
  bool ok = false;  // false: no child exists, see `error`
  std::string error;
  pid_t pid = -1;
  unique_fd pidfd;
  unique_fd out_r, err_r;  // non-blocking
  std::optional<int> exec_errno;
  const char* exec_step = "execvp";  // the child's call that failed, with exec_errno
  bool box_attach_failed = false;  // the child was killed before it could exec
};

// On return with ok == true the child is unreaped and either running the
// command or already exited after a failed exec (exec_errno set). If
// `box_procs_fd` is valid, the child joins that cgroup before it can fork.
Spawned spawn_child(const std::vector<std::string>& argv, StdinPolicy stdin_policy,
                    int box_procs_fd);

}  // namespace corral
