#include "terminate.hpp"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <vector>

#include "procscan.hpp"

namespace corral {
namespace {

constexpr int64_t kGraceTickMs = 20;
constexpr int64_t kVerifyTickMs = 10;

int tick(TimePoint deadline, int64_t max_ms) {
  return static_cast<int>(std::min(ms_until(deadline), max_ms));
}

void signal_all(const std::vector<pid_t>& live, int sig, const FallbackTarget& target) {
  for (pid_t pid : live) safe_signal(pid, sig, target);
}

// Reaps every exited child. Returns true once waitpid reports ECHILD.
bool reap_all(pid_t main_pid, RunResult& result, bool& status_captured) {
  for (;;) {
    int status;
    const pid_t pid = waitpid(-1, &status, WNOHANG);
    if (pid > 0) {
      if (pid != main_pid) {
        ++result.orphans_reaped;
      } else if (!status_captured) {
        // It died between the caller's WNOWAIT check and this waitpid.
        if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
        if (WIFSIGNALED(status)) result.term_signal = WTERMSIG(status);
        status_captured = true;
      }
      continue;
    }
    if (pid == 0) return false;
    if (errno == EINTR) continue;
    return errno == ECHILD;
  }
}

}  // namespace

void terminate_and_verify(Spawned& child, OutputRouter& out, RunResult& result,
                          bool& status_captured, std::chrono::milliseconds grace,
                          std::chrono::milliseconds verify_timeout, TimePoint start) {
  const FallbackTarget target{getpid(), child.pid};
  std::vector<pid_t> live = fallback_live_set(target);
  result.termination.leftovers_at_trigger = static_cast<uint64_t>(
      std::count_if(live.begin(), live.end(), [&](pid_t pid) { return pid != child.pid; }));

  // Phase A, graceful. The main child is unreaped, so its process group id
  // cannot have been recycled and kill(-pid) reaches only its group.
  if (!live.empty() && grace.count() > 0) {
    kill(-child.pid, SIGTERM);  // ESRCH: the group is already empty
    signal_all(live, SIGTERM, target);
    result.termination.term_sent_ms = ms_since(start);
    const TimePoint grace_end = deadline_after(now(), grace);
    while (!live.empty() && now() < grace_end) {
      service(child, out, result, status_captured, tick(grace_end, kGraceTickMs));
      live = fallback_live_set(target);
    }
  }

  // Phase B, forceful.
  if (!live.empty()) {
    kill(-child.pid, SIGKILL);
    signal_all(live, SIGKILL, target);
    result.termination.kill_sent_ms = ms_since(start);
  }

  // Phase C, drain and verify. The subreaper makes every orphan corral's
  // child, so repeated passes also catch descendants that left the group or
  // session once their ancestors died.
  const TimePoint verify_end = deadline_after(now(), verify_timeout);
  for (;;) {
    live = fallback_live_set(target);
    signal_all(live, SIGKILL, target);
    // Reaping comes after signaling: signaling a direct child by pid is
    // race-free only while it is unreaped.
    if (!status_captured) service(child, out, result, status_captured, 0);
    const bool echild = reap_all(child.pid, result, status_captured);
    if (live.empty() && echild) break;
    if (now() >= verify_end) {
      result.survivors = fallback_live_set(target);
      break;
    }
    service(child, out, result, status_captured, tick(verify_end, kVerifyTickMs));
  }
  result.orphans_remaining = result.survivors.size();
  result.termination.verified_ms = ms_since(start);
}

}  // namespace corral
