#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "clock.hpp"
#include "runner.hpp"
#include "spawn.hpp"
#include "unique_fd.hpp"

namespace corral {

enum class Stream { Stdout = 0, Stderr = 1 };

class OutputRouter {
 public:
  // `cap` limits stdout plus stderr bytes together (--max-output).
  OutputRouter(OutputMode mode, std::optional<uint64_t> cap, RunResult& result);

  // Reads from `fd` until EAGAIN. Closes `fd` and returns false at EOF.
  bool pump(Stream stream, unique_fd& fd);

  // True once the command wrote more bytes than the cap allows.
  bool cap_reached() const { return result_.output.truncated; }

 private:
  void deliver(Stream stream, const char* data, size_t len);

  OutputMode mode_;
  std::optional<uint64_t> cap_;
  RunResult& result_;
  bool forwarding_[2] = {true, true};  // false after a failed write to that stream
  std::vector<char> buf_;
};

// Captures the main child's status with waitid(WNOWAIT) (the child stays
// unreaped). Returns Exited or Signaled, or SetupFailed if waitid failed.
ExitReason capture_status(const Spawned& child, RunResult& result);

// Returns the trigger that ends the running phase.
ExitReason supervise(Spawned& child, OutputRouter& out, RunResult& result,
                     std::optional<TimePoint> deadline, int cancel_fd);

// Waits up to `timeout_ms` for output or main child exit and handles both.
// The pidfd is watched only while `status_captured` is false.
void service(Spawned& child, OutputRouter& out, RunResult& result, bool& status_captured,
             int timeout_ms);

}  // namespace corral
