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

}  // namespace corral
