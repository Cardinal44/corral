#pragma once

#include <chrono>

#include "clock.hpp"
#include "runner.hpp"
#include "spawn.hpp"
#include "supervise.hpp"

namespace corral {

// Fallback mode: graceful SIGTERM, forceful SIGKILL, then drain
// and verify until the live set is empty and waitpid(-1) reports ECHILD.
// Fills result.termination, orphans_reaped, and survivors, and sets
// `status_captured` if it reaps the main child.
void terminate_and_verify(Spawned& child, OutputRouter& out, RunResult& result,
                          bool& status_captured, std::chrono::milliseconds grace,
                          std::chrono::milliseconds verify_timeout, TimePoint start);

}  // namespace corral
