#include "supervise.hpp"

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>

namespace corral {
namespace {

constexpr size_t kReadChunk = 64 * 1024;

bool write_all(int fd, const char* data, size_t len) {
  while (len > 0) {
    ssize_t w = write(fd, data, len);
    if (w > 0) {
      data += w;
      len -= static_cast<size_t>(w);
      continue;
    }
    if (w < 0 && errno == EINTR) continue;
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // Whoever started corral left its stdout/stderr non-blocking.
      pollfd pfd{fd, POLLOUT, 0};
      if (poll(&pfd, 1, -1) < 0 && errno != EINTR) return false;
      continue;
    }
    return false;
  }
  return true;
}

int poll_timeout(std::optional<TimePoint> deadline) {
  if (!deadline) return -1;
  return static_cast<int>(std::min<int64_t>(ms_until(*deadline), INT_MAX));
}

}  // namespace

OutputRouter::OutputRouter(OutputMode mode, std::optional<uint64_t> cap, RunResult& result)
    : mode_(mode), cap_(cap), result_(result), buf_(kReadChunk) {}

bool OutputRouter::pump(Stream stream, unique_fd& fd) {
  for (;;) {
    ssize_t n = read(fd.get(), buf_.data(), buf_.size());
    if (n > 0) {
      deliver(stream, buf_.data(), static_cast<size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
    fd.reset();  // EOF, or a read error: the stream is finished either way
    return false;
  }
}

void OutputRouter::deliver(Stream stream, const char* data, size_t len) {
  if (cap_) {
    // Bytes past the cap are dropped, but the caller keeps reading: a full
    // pipe would also block the command's cleanup output after SIGTERM.
    const uint64_t room = *cap_ - (result_.output.stdout_bytes + result_.output.stderr_bytes);
    if (len > room) {
      len = static_cast<size_t>(room);
      result_.output.truncated = true;
    }
  }
  const int idx = static_cast<int>(stream);
  if (stream == Stream::Stdout) {
    result_.output.stdout_bytes += len;
  } else {
    result_.output.stderr_bytes += len;
  }
  switch (mode_) {
    case OutputMode::Forward:
      if (forwarding_[idx] && !write_all(idx == 0 ? STDOUT_FILENO : STDERR_FILENO, data, len))
        forwarding_[idx] = false;
      break;
    case OutputMode::Capture:
      (stream == Stream::Stdout ? result_.stdout_data : result_.stderr_data).append(data, len);
      break;
    case OutputMode::Discard:
      break;
  }
}

ExitReason capture_status(const Spawned& child, RunResult& result) {
  siginfo_t info;
  std::memset(&info, 0, sizeof info);
  int rc;
  do {
    rc = waitid(static_cast<idtype_t>(P_PIDFD), static_cast<id_t>(child.pidfd.get()), &info,
                WEXITED | WNOWAIT);
  } while (rc < 0 && errno == EINTR);
  if (rc < 0) {
    result.error = std::string("waitid: ") + std::strerror(errno);
    return ExitReason::SetupFailed;
  }
  if (info.si_code == CLD_EXITED) {
    result.exit_code = info.si_status;
    return ExitReason::Exited;
  }
  result.term_signal = info.si_status;  // CLD_KILLED or CLD_DUMPED
  return ExitReason::Signaled;
}

ExitReason supervise(Spawned& child, OutputRouter& out, RunResult& result,
                     std::optional<TimePoint> deadline, int cancel_fd) {
  for (;;) {
    // Recomputed every pass, so EINTR and early wakeups cannot stretch the deadline.
    const int timeout = poll_timeout(deadline);
    if (timeout == 0) return ExitReason::Timeout;

    // A closed pipe has fd -1, which poll() ignores.
    pollfd fds[4] = {
        {child.pidfd.get(), POLLIN, 0},
        {child.out_r.get(), POLLIN, 0},
        {child.err_r.get(), POLLIN, 0},
        {cancel_fd, POLLIN, 0},
    };
    if (poll(fds, 4, timeout) < 0) {
      if (errno == EINTR) continue;
      if (errno == ENOMEM) {
        sleep_ms(10);
        continue;
      }
      // EFAULT and EINVAL are impossible with this fixed four-entry array.
      std::abort();
    }
    // Pipes before the pidfd: output the child wrote before exiting must not
    // be lost when both become ready in the same poll, and a cap it overran
    // before exiting is the earlier trigger.
    if (fds[1].revents != 0) out.pump(Stream::Stdout, child.out_r);
    if (fds[2].revents != 0) out.pump(Stream::Stderr, child.err_r);
    if (out.cap_reached()) return ExitReason::OutputLimit;
    // Ends on main child exit without waiting for pipe EOF: a descendant may
    // hold the write ends open indefinitely.
    if (fds[0].revents != 0) return capture_status(child, result);
    if (fds[3].revents != 0) {
      result.cancelled = true;
      return ExitReason::Cancelled;
    }
  }
}

void service(Spawned& child, OutputRouter& out, RunResult& result, bool& status_captured,
             int timeout_ms) {
  pollfd fds[3] = {
      {status_captured ? -1 : child.pidfd.get(), POLLIN, 0},
      {child.out_r.get(), POLLIN, 0},
      {child.err_r.get(), POLLIN, 0},
  };
  // Callers loop on their own deadlines, so a failed or interrupted poll just
  // ends this pass early.
  if (poll(fds, 3, timeout_ms) <= 0) return;
  if (fds[1].revents != 0) out.pump(Stream::Stdout, child.out_r);
  if (fds[2].revents != 0) out.pump(Stream::Stderr, child.err_r);
  if (fds[0].revents != 0) {
    capture_status(child, result);
    status_captured = true;
  }
}

}  // namespace corral
