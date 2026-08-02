#include "runner.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/utsname.h>
#include <time.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

#include "cgroup.hpp"
#include "clock.hpp"
#include "spawn.hpp"
#include "supervise.hpp"
#include "terminate.hpp"
#include "unique_fd.hpp"

#ifndef CORRAL_VERSION
#define CORRAL_VERSION "0.0.0"
#endif

namespace corral {
namespace {

std::string iso8601_utc_now() {
  timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) < 0) return "";
  tm t;
  if (gmtime_r(&ts.tv_sec, &t) == nullptr) return "";
  char buf[96];  // sized for -Wformat-truncation's worst case
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ", t.tm_year + 1900,
                t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, ts.tv_nsec / 1000000L);
  return buf;
}

std::string kernel_release() {
  utsname u;
  if (uname(&u) < 0) return "";
  return u.release;
}

// Orphaned descendants reparent to corral instead of init, so it can reap them.
bool ensure_subreaper(std::string& error) {
  static bool done = false;
  if (done) return true;
  if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) < 0) {
    error = std::string("prctl(PR_SET_CHILD_SUBREAPER): ") + std::strerror(errno);
    return false;
  }
  done = true;
  return true;
}

// Without a box, --mem and --pids have no effect. Only an automatic fallback
// warns: with --cgroup-mode=fallback the user already chose that.
void fall_back(const RunSpec& spec, std::string reason, RunResult& r) {
  r.mode = Mode::Fallback;
  r.mode_reason = std::move(reason);
  if (spec.mem_bytes) r.limits_enforced.mem = false;
  if (spec.pids) r.limits_enforced.pids = false;
  if ((spec.mem_bytes || spec.pids) && spec.mode == ModeRequest::Auto) {
    r.warnings.push_back(*r.mode_reason + ": fallback mode, --mem and --pids are not enforced");
  }
}

// Spawns, supervises, terminates, verifies, and removes the box. nullopt: the
// child could not join the box, so nothing ran and the box is already removed.
std::optional<ExitReason> execute(const RunSpec& spec, TimePoint start, const Box* box,
                                  RunResult& r) {
  std::string ignored;
  unique_fd procs;
  if (box) {
    const std::string procs_path = box->path() + "/cgroup.procs";
    procs.reset(open(procs_path.c_str(), O_WRONLY | O_CLOEXEC));
    if (!procs) {
      const int err = errno;
      r.error = "open " + procs_path + ": " + std::strerror(err);
      box->remove(now(), ignored);
      return std::nullopt;
    }
  }
  Spawned child = spawn_child(spec.argv, spec.stdin_policy, procs.get());
  procs.reset();
  if (!child.ok) {
    r.error = child.error;
    if (box) {
      box->remove(now(), ignored);
      if (child.box_attach_failed) {
        r.error = box->path() + ": " + r.error;
        return std::nullopt;
      }
    }
    return ExitReason::SetupFailed;
  }

  OutputRouter out(spec.output_mode, spec.max_output_bytes, r);
  ExitReason trigger;
  bool status_captured;
  if (child.exec_errno) {
    r.spawn_errno = *child.exec_errno;
    if (std::strcmp(child.exec_step, "execvp") == 0) {
      r.error = "cannot execute '" + spec.argv[0] + "': " + std::strerror(*child.exec_errno);
    } else {
      r.error = std::string("child setup failed in ") + child.exec_step + " before executing '" +
                spec.argv[0] + "': " + std::strerror(*child.exec_errno);
    }
    trigger = ExitReason::SpawnFailed;
    status_captured = true;  // its exit code is corral's, not the command's
  } else {
    std::optional<TimePoint> deadline;
    if (spec.wall) deadline = deadline_after(start, *spec.wall);
    trigger = supervise(child, out, r, deadline, spec.cancel_fd);
    status_captured = trigger != ExitReason::Timeout && trigger != ExitReason::Cancelled &&
                      trigger != ExitReason::OutputLimit;
  }
  r.termination.trigger_ms = ms_since(start);

  terminate_and_verify(child, out, r, status_captured, spec.grace, spec.verify_timeout, start,
                       box);
  // Every writer is dead now, so this collects the last buffered output.
  service(child, out, r, status_captured, 0);

  if (!r.survivors.empty()) {
    if (!r.error.empty()) r.error += "; ";
    r.error += std::to_string(r.survivors.size()) + " process(es) survived verification:";
    for (pid_t pid : r.survivors) r.error += " " + std::to_string(pid);
  }
  // An OOM kill ends the command by a signal, but the memory limit is the cause.
  if ((trigger == ExitReason::Exited || trigger == ExitReason::Signaled) &&
      r.limit_events.oom_kills.value_or(0) > 0) {
    trigger = ExitReason::MemoryLimit;
  }
  return trigger;
}

}  // namespace

RunResult run(const RunSpec& spec) {
  const TimePoint start = now();
  RunResult r;
  r.corral_version = CORRAL_VERSION;
  r.argv = spec.argv;
  r.started_at = iso8601_utc_now();
  r.kernel = kernel_release();

  if (spec.wall) r.limits.wall_ms = spec.wall->count();
  r.limits.grace_ms = spec.grace.count();
  r.limits.verify_timeout_ms = spec.verify_timeout.count();
  r.limits.mem_bytes = spec.mem_bytes;
  r.limits.pids = spec.pids;
  r.limits.output_bytes = spec.max_output_bytes;
  if (spec.wall) r.limits_enforced.wall = true;
  if (spec.max_output_bytes) r.limits_enforced.output = true;

  auto finish = [&r, start](ExitReason reason) {
    r.exit_reason = reason;
    r.duration_ms = ms_since(start);
    r.corral_exit_code = exit_code_for(r);
    return std::move(r);
  };

  if (spec.argv.empty()) {
    r.error = "no command given";
    return finish(ExitReason::SetupFailed);
  }
  if (!ensure_subreaper(r.error)) return finish(ExitReason::SetupFailed);

  std::optional<Box> box;
  if (spec.mode == ModeRequest::Fallback) {
    fall_back(spec, "forced_by_flag", r);
  } else {
    BoxSetup setup = create_box(spec);
    if (setup.box) {
      box = std::move(setup.box);
      r.mode = Mode::Enforced;
      r.cgroup_path = box->path();
      r.limits_enforced.mem = setup.mem_applied;
      r.limits_enforced.pids = setup.pids_applied;
      r.warnings = std::move(setup.warnings);
    } else if (spec.mode == ModeRequest::Enforced) {
      r.mode_reason = setup.reason;
      r.error = "enforced mode unavailable (" + setup.reason + "): " + setup.error;
      return finish(ExitReason::SetupFailed);
    } else {
      fall_back(spec, setup.reason, r);
    }
  }

  std::optional<ExitReason> trigger = execute(spec, start, box ? &*box : nullptr, r);
  if (!trigger) {
    if (spec.mode == ModeRequest::Enforced) return finish(ExitReason::SetupFailed);
    // Auto mode: the child never ran, so the whole run starts again without a box.
    r.warnings.push_back("cgroup_setup_error: " + r.error);
    r.error.clear();
    r.cgroup_path.reset();
    fall_back(spec, "cgroup_setup_error", r);
    trigger = execute(spec, start, nullptr, r);
  }
  return finish(*trigger);
}

int exit_code_for(const RunResult& r) {
  if (!r.survivors.empty() || r.verify_failed) return 120;
  switch (r.exit_reason) {
    case ExitReason::Exited:      return r.exit_code.value_or(0);
    case ExitReason::Signaled:    return 128 + r.term_signal.value_or(0);
    case ExitReason::Timeout:     return 124;
    case ExitReason::MemoryLimit: return 121;
    case ExitReason::OutputLimit: return 122;
    case ExitReason::Cancelled:   return 128 + r.cancel_signal.value_or(SIGTERM);
    case ExitReason::SpawnFailed: return r.spawn_errno == ENOENT ? 127 : 126;
    case ExitReason::SetupFailed: return 125;
  }
  return 125;
}

}  // namespace corral
