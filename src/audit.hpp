#pragma once

#include <string>
#include <string_view>

#include "runner.hpp"

namespace corral {

// A JSON string literal, quotes included. Escapes '"', '\\', and every byte
// below 0x20; bytes 0x80 and above pass through unchanged.
std::string json_quote(std::string_view s);

// The audit record: one JSON object on one line, then a newline.
std::string audit_json(const RunResult& r);

// The human summary line, newline included.
std::string summary_line(const RunResult& r);

}  // namespace corral
