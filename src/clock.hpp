#pragma once

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

// Rounded up, so a poll() timeout of this length never wakes before `deadline`.
inline int64_t ms_until(TimePoint deadline) {
  const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - now()).count();
  return left > 0 ? left : 0;
}

}  // namespace corral
