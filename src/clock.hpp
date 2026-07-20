#pragma once

#include <time.h>

#include <cerrno>
#include <chrono>
#include <cstdint>

namespace corral {

using Clock = std::chrono::steady_clock;  // CLOCK_MONOTONIC on Linux
using TimePoint = Clock::time_point;

inline TimePoint now() { return Clock::now(); }

inline int64_t ms_between(TimePoint start, TimePoint t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(t - start).count();
}

inline int64_t ms_since(TimePoint start) { return ms_between(start, now()); }

// Saturates instead of overflowing for huge durations (DURATION allows up to
// INT64_MAX ms, but the clock counts nanoseconds).
inline TimePoint deadline_after(TimePoint t, std::chrono::milliseconds d) {
  if (d >= std::chrono::duration_cast<std::chrono::milliseconds>(TimePoint::max() - t))
    return TimePoint::max();
  return t + d;
}

// For short pauses only: `ms` must be below 1000.
inline void sleep_ms(long ms) {
  timespec ts{0, ms * 1000000L};
  while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
  }
}

// Rounded up, so a poll() timeout of this length never wakes before `deadline`.
inline int64_t ms_until(TimePoint deadline) {
  const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - now()).count();
  return left > 0 ? left : 0;
}

}  // namespace corral
