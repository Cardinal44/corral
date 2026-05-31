#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runner.hpp"

namespace corral {

// DURATION: integer with suffix ms, s, m, or h; bare integer is seconds; >= 0.
std::optional<std::chrono::milliseconds> parse_duration(std::string_view s);
// SIZE: integer with optional suffix K, M, or G (powers of 1024); > 0.
std::optional<uint64_t> parse_size(std::string_view s);
// COUNT: positive integer.
std::optional<uint64_t> parse_count(std::string_view s);

struct CliOptions {
  RunSpec spec;
  std::optional<std::string> json_path;
  bool quiet = false;
  bool help = false;
  bool version = false;
};

struct CliParse {
  std::optional<CliOptions> options;
  std::string error;
};

// `args` excludes argv[0].
CliParse parse_args(const std::vector<std::string>& args);

extern const char* const kUsage;

}  // namespace corral
