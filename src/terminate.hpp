#pragma once

#include <chrono>

#include "cgroup.hpp"
#include "clock.hpp"
#include "runner.hpp"
#include "spawn.hpp"
#include "supervise.hpp"

namespace corral {

// SIGTERM, then SIGKILL (and cgroup.kill with a box), then drains until the
// live set is empty, the box is unpopulated, and waitpid(-1) reports ECHILD.
// Then reads the box's limit events and removes it. Sets `status_captured` if
// it reaps the main child.
void terminate_and_verify(Spawned& child, OutputRouter& out, RunResult& result,
                          bool& status_captured, std::chrono::milliseconds grace,
                          std::chrono::milliseconds verify_timeout, TimePoint start,
                          const Box* box);

}  // namespace corral
