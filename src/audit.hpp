#pragma once

#include <string>
#include <string_view>

#include "runner.hpp"

namespace corral {

// Quoted JSON string. Bytes 0x80 and above pass through unescaped.
std::string json_quote(std::string_view s);

// One JSON object on one line, newline included.
std::string audit_json(const RunResult& r);

std::string summary_line(const RunResult& r);

}  // namespace corral
