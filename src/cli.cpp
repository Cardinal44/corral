#include "cli.hpp"

#include <cstddef>
#include <limits>
#include <utility>

namespace corral {
namespace {

std::optional<size_t> parse_digits(std::string_view s, uint64_t& value) {
  value = 0;
  size_t i = 0;
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
    const uint64_t d = static_cast<uint64_t>(s[i] - '0');
    if (value > (std::numeric_limits<uint64_t>::max() - d) / 10) return std::nullopt;
    value = value * 10 + d;
  }
  return i;
}

std::optional<uint64_t> checked_mul(uint64_t a, uint64_t b, uint64_t max) {
  if (a != 0 && b > max / a) return std::nullopt;
  return a * b;
}

}  // namespace

std::optional<std::chrono::milliseconds> parse_duration(std::string_view s) {
  uint64_t value;
  const auto digits = parse_digits(s, value);
  if (!digits || *digits == 0) return std::nullopt;
  const std::string_view suffix = s.substr(*digits);
  uint64_t unit;
  if (suffix.empty() || suffix == "s") unit = 1000;
  else if (suffix == "ms") unit = 1;
  else if (suffix == "m") unit = 60 * 1000;
  else if (suffix == "h") unit = 60 * 60 * 1000;
  else return std::nullopt;
  using Rep = std::chrono::milliseconds::rep;
  const auto ms = checked_mul(value, unit, static_cast<uint64_t>(std::numeric_limits<Rep>::max()));
  if (!ms) return std::nullopt;
  return std::chrono::milliseconds(static_cast<Rep>(*ms));
}

std::optional<uint64_t> parse_size(std::string_view s) {
  uint64_t value;
  const auto digits = parse_digits(s, value);
  if (!digits || *digits == 0) return std::nullopt;
  const std::string_view suffix = s.substr(*digits);
  uint64_t unit;
  if (suffix.empty()) unit = 1;
  else if (suffix == "K") unit = 1ULL << 10;
  else if (suffix == "M") unit = 1ULL << 20;
  else if (suffix == "G") unit = 1ULL << 30;
  else return std::nullopt;
  const auto bytes = checked_mul(value, unit, std::numeric_limits<uint64_t>::max());
  if (!bytes || *bytes == 0) return std::nullopt;
  return bytes;
}

std::optional<uint64_t> parse_count(std::string_view s) {
  uint64_t value;
  const auto digits = parse_digits(s, value);
  if (!digits || *digits == 0 || *digits != s.size() || value == 0) return std::nullopt;
  return value;
}

const char* const kUsage =
    "usage: corral [options] [--] command [args...]\n"
    "\n"
    "options:\n"
    "  --wall DURATION            wall-clock limit for the run\n"
    "  --grace DURATION           time between SIGTERM and SIGKILL (default 2s; 0 skips SIGTERM)\n"
    "  --verify-timeout DURATION  maximum time to drain and verify after SIGKILL (default 2s)\n"
    "  --mem SIZE                 memory limit (enforced mode only)\n"
    "  --pids COUNT               maximum number of processes (enforced mode only)\n"
    "  --max-output SIZE          combined stdout plus stderr byte cap\n"
    "  --cgroup-mode MODE         auto, enforced, or fallback (default auto)\n"
    "  --cgroup-parent PATH       delegated cgroup under which to create boxes\n"
    "  --stdin POLICY             null or inherit (default null)\n"
    "  --json PATH                write the audit record to PATH\n"
    "  --quiet                    suppress the human summary line\n"
    "  --version                  print version and exit\n"
    "  --help                     print this help and exit\n"
    "\n"
    "DURATION: integer with ms, s, m, or h suffix (bare integer = seconds)\n"
    "SIZE:     integer with optional K, M, or G suffix (bare integer = bytes)\n";

CliParse parse_args(const std::vector<std::string>& args) {
  CliParse out;
  CliOptions opts;
  auto usage_error = [&out](std::string msg) {
    out.error = std::move(msg);
    return std::move(out);
  };

  size_t i = 0;
  for (; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "--") {
      ++i;
      break;
    }
    if (arg.size() < 2 || arg[0] != '-') break;  // first non-option ("-" included)
    if (arg[1] != '-') return usage_error("unknown option: " + arg);

    const size_t eq = arg.find('=');
    const std::string name = arg.substr(0, eq);
    const bool has_inline = eq != std::string::npos;

    if (name == "--quiet" || name == "--version" || name == "--help") {
      if (has_inline) return usage_error("option " + name + " takes no argument");
      if (name == "--quiet") opts.quiet = true;
      else if (name == "--version") opts.version = true;
      else opts.help = true;
      continue;
    }

    std::string value;
    if (has_inline) {
      value = arg.substr(eq + 1);
    } else if (i + 1 < args.size()) {
      value = args[++i];
    } else {
      return usage_error("option " + name + " requires an argument");
    }
    const std::string bad = "invalid value for " + name + ": '" + value + "'";

    RunSpec& spec = opts.spec;
    if (name == "--wall" || name == "--grace" || name == "--verify-timeout") {
      auto d = parse_duration(value);
      if (!d) return usage_error(bad);
      if (name == "--wall") spec.wall = *d;
      else if (name == "--grace") spec.grace = *d;
      else spec.verify_timeout = *d;
    } else if (name == "--mem" || name == "--max-output") {
      auto n = parse_size(value);
      if (!n) return usage_error(bad);
      (name == "--mem" ? spec.mem_bytes : spec.max_output_bytes) = *n;
    } else if (name == "--pids") {
      auto n = parse_count(value);
      if (!n) return usage_error(bad);
      spec.pids = *n;
    } else if (name == "--cgroup-mode") {
      if (value == "auto") spec.mode = ModeRequest::Auto;
      else if (value == "enforced") spec.mode = ModeRequest::Enforced;
      else if (value == "fallback") spec.mode = ModeRequest::Fallback;
      else return usage_error(bad);
    } else if (name == "--cgroup-parent") {
      if (value.empty()) return usage_error(bad);
      spec.cgroup_parent = value;
    } else if (name == "--stdin") {
      if (value == "null") spec.stdin_policy = StdinPolicy::Null;
      else if (value == "inherit") spec.stdin_policy = StdinPolicy::Inherit;
      else return usage_error(bad);
    } else if (name == "--json") {
      if (value.empty()) return usage_error(bad);
      opts.json_path = value;
    } else {
      return usage_error("unknown option: " + name);
    }
  }

  opts.spec.argv.assign(args.begin() + static_cast<std::ptrdiff_t>(i), args.end());
  if (opts.spec.argv.empty() && !opts.help && !opts.version)
    return usage_error("missing command");

  out.options = std::move(opts);
  return out;
}

}  // namespace corral
