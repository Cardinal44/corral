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
};

// On return with ok == true the child is unreaped and either running the command or already exited after a
// failed exec (exec_errno set).
Spawned spawn_child(const std::vector<std::string>& argv, StdinPolicy stdin_policy);

}  // namespace corral
