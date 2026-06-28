#include "audit.hpp"

#include <sys/types.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace corral {
namespace {

using Fields = std::vector<std::pair<const char*, std::string>>;

std::string object(const Fields& fields) {
  std::string out = "{";
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i > 0) out += ", ";
    out += '"';
    out += fields[i].first;
    out += "\": ";
    out += fields[i].second;
  }
  return out + "}";
}

std::string array(const std::vector<std::string>& items) {
  std::string out = "[";
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) out += ", ";
    out += items[i];
  }
  return out + "]";
}

template <typename T>
std::string number(T v) {
  return std::to_string(v);
}

template <typename T>
std::string number_or_null(const std::optional<T>& v) {
  return v ? std::to_string(*v) : "null";
}

std::string boolean_or_null(std::optional<bool> v) {
  if (!v) return "null";
  return *v ? "true" : "false";
}

std::string string_or_null(const std::optional<std::string>& v) {
  return v ? json_quote(*v) : "null";
}

const char* mode_name(Mode m) { return m == Mode::Enforced ? "enforced" : "fallback"; }

const char* exit_reason_name(ExitReason reason) {
  switch (reason) {
    case ExitReason::Exited:      return "exited";
    case ExitReason::Signaled:    return "signaled";
    case ExitReason::Timeout:     return "timeout";
    case ExitReason::MemoryLimit: return "memory_limit";
    case ExitReason::OutputLimit: return "output_limit";
    case ExitReason::Cancelled:   return "cancelled";
    case ExitReason::SpawnFailed: return "spawn_failed";
    case ExitReason::SetupFailed: return "setup_failed";
  }
  return "setup_failed";
}

}  // namespace

std::string json_quote(std::string_view s) {
  static const char kHex[] = "0123456789abcdef";
  std::string out = "\"";
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (u < 0x20) {
          out += "\\u00";
          out += kHex[u >> 4];
          out += kHex[u & 0xf];
        } else {
          out += c;
        }
    }
  }
  return out + "\"";
}

std::string audit_json(const RunResult& r) {
  std::vector<std::string> argv;
  for (const std::string& arg : r.argv) argv.push_back(json_quote(arg));
  std::vector<std::string> survivors;
  for (pid_t pid : r.survivors) survivors.push_back(number(pid));

  const Limits& l = r.limits;
  const LimitsEnforced& le = r.limits_enforced;
  const Termination& t = r.termination;
  const LimitEvents& ev = r.limit_events;
  return object({
      {"schema_version", number(r.schema_version)},
      {"corral_version", json_quote(r.corral_version)},
      {"argv", array(argv)},
      {"mode", json_quote(mode_name(r.mode))},
      {"mode_reason", string_or_null(r.mode_reason)},
      {"limits", object({
          {"wall_ms", number_or_null(l.wall_ms)},
          {"grace_ms", number(l.grace_ms)},
          {"verify_timeout_ms", number(l.verify_timeout_ms)},
          {"mem_bytes", number_or_null(l.mem_bytes)},
          {"pids", number_or_null(l.pids)},
          {"output_bytes", number_or_null(l.output_bytes)},
      })},
      {"limits_enforced", object({
          {"wall", boolean_or_null(le.wall)},
          {"mem", boolean_or_null(le.mem)},
          {"pids", boolean_or_null(le.pids)},
          {"output", boolean_or_null(le.output)},
      })},
      {"started_at", json_quote(r.started_at)},
      {"duration_ms", number(r.duration_ms)},
      {"exit_reason", json_quote(exit_reason_name(r.exit_reason))},
      {"exit_code", number_or_null(r.exit_code)},
      {"term_signal", number_or_null(r.term_signal)},
      {"cancel_signal", number_or_null(r.cancel_signal)},
      {"spawn_errno", number_or_null(r.spawn_errno)},
      {"corral_exit_code", number(r.corral_exit_code)},
      {"output", object({
          {"stdout_bytes", number(r.output.stdout_bytes)},
          {"stderr_bytes", number(r.output.stderr_bytes)},
          {"truncated", r.output.truncated ? "true" : "false"},
      })},
      {"termination", object({
          {"trigger_ms", number(t.trigger_ms)},
          {"leftovers_at_trigger", number(t.leftovers_at_trigger)},
          {"term_sent_ms", number_or_null(t.term_sent_ms)},
          {"kill_sent_ms", number_or_null(t.kill_sent_ms)},
          {"verified_ms", number(t.verified_ms)},
      })},
      {"orphans_reaped", number(r.orphans_reaped)},
      {"orphans_remaining", number(r.orphans_remaining)},
      {"survivors", array(survivors)},
      {"limit_events", object({
          {"oom_kills", number_or_null(ev.oom_kills)},
          {"pids_max_hits", number_or_null(ev.pids_max_hits)},
          {"peak_mem_bytes", number_or_null(ev.peak_mem_bytes)},
      })},
      {"cgroup_path", string_or_null(r.cgroup_path)},
      {"kernel", json_quote(r.kernel)},
  }) + "\n";
}

std::string summary_line(const RunResult& r) {
  const int64_t ms = r.duration_ms;
  std::string millis = std::to_string(ms % 1000);
  millis.insert(0, 3 - millis.size(), '0');
  return std::string("corral: ") + exit_reason_name(r.exit_reason) +
         " code=" + std::to_string(r.corral_exit_code) +
         " in " + std::to_string(ms / 1000) + "." + millis + "s" +
         " mode=" + mode_name(r.mode) +
         " leftovers_killed=" + std::to_string(r.termination.leftovers_at_trigger) + "\n";
}

}  // namespace corral
