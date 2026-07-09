#pragma once

#include <chrono>

#include "clock.hpp"
#include "runner.hpp"
#include "spawn.hpp"
#include "supervise.hpp"

namespace corral {

// SIGTERM, then SIGKILL, then drains until the live set is empty and
// waitpid(-1) reports ECHILD. Sets `status_captured` if it reaps the main child.
void terminate_and_verify(Spawned& child, OutputRouter& out, RunResult& result,
                          bool& status_captured, std::chrono::milliseconds grace,
                          std::chrono::milliseconds verify_timeout, TimePoint start);

}  // namespace corral
