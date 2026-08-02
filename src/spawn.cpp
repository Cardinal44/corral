#include "spawn.hpp"

#include <fcntl.h>
#include <signal.h>
// glibc 2.36-2.39 ship <sys/pidfd.h> without __BEGIN_DECLS, so C++ would
// mangle pidfd_open and fail to link. Its other includes are C++-safe.
extern "C" {
#include <sys/pidfd.h>
}
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <initializer_list>
#include <utility>

namespace corral {
namespace {

std::string sys_error(const char* what, int err) {
  return std::string(what) + ": " + std::strerror(err);
}

bool make_pipe(unique_fd& r, unique_fd& w) {
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) < 0) return false;
  r.reset(fds[0]);
  w.reset(fds[1]);
  return true;
}

bool set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0) return false;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// The child dup2()s onto fds 0-2, so every fd it uses must be >= 3. A lower fd
// (the host closed a standard stream) would be clobbered by dup2, or dup2 would
// be a no-op that leaves FD_CLOEXEC set.
bool lift_above_stdio(unique_fd& fd) {
  if (fd.get() >= 3) return true;
  int moved = fcntl(fd.get(), F_DUPFD_CLOEXEC, 3);
  if (moved < 0) return false;
  fd.reset(moved);
  return true;
}

// The child's calls, in order. The child reports the failing one by index,
// because it may not build a string.
constexpr const char* kChildSteps[] = {"setsid", "prctl(PR_SET_PDEATHSIG)", "sigprocmask",
                                       "sigaction", "dup2", "close_range", "execvp"};
enum ChildStep : int { kSetsid, kPrctl, kSigprocmask, kSigaction, kDup2, kCloseRange, kExecvp };

[[noreturn]] void child_fail(int execerr_w, ChildStep step, int code) {
  const int report[2] = {step, errno};
  // Nobody can act on a failed report: the parent then sees EOF, and the exit
  // status still says the child failed.
  ssize_t n = write(execerr_w, report, sizeof report);
  (void)n;
  _exit(code);
}

void reap_now(pid_t pid) {
  int status;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
}

}  // namespace

Spawned spawn_child(const std::vector<std::string>& argv, StdinPolicy stdin_policy,
                    int box_procs_fd) {
  Spawned s;
  auto fail = [&s](const char* what) {
    s.error = sys_error(what, errno);
    return std::move(s);
  };

  unique_fd out_r, out_w, err_r, err_w, go_r, go_w, execerr_r, execerr_w;
  if (!make_pipe(out_r, out_w) || !make_pipe(err_r, err_w) ||
      !make_pipe(go_r, go_w) || !make_pipe(execerr_r, execerr_w))
    return fail("pipe2");
  if (!set_nonblock(out_r.get()) || !set_nonblock(err_r.get()))
    return fail("fcntl(O_NONBLOCK)");

  unique_fd stdin_src;
  if (stdin_policy == StdinPolicy::Null) {
    stdin_src.reset(open("/dev/null", O_RDONLY | O_CLOEXEC));
    if (!stdin_src) return fail("open /dev/null");
  }

  for (unique_fd* fd : {&stdin_src, &out_w, &err_w, &go_r, &execerr_w}) {
    if (fd->valid() && !lift_above_stdio(*fd)) return fail("fcntl(F_DUPFD_CLOEXEC)");
  }

  std::vector<char*> cargv;
  cargv.reserve(argv.size() + 1);
  for (const std::string& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
  cargv.push_back(nullptr);

  // Prepared before fork: the child may only make async-signal-safe calls.
  sigset_t empty_mask;
  sigemptyset(&empty_mask);
  struct sigaction dfl;
  std::memset(&dfl, 0, sizeof dfl);
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  static constexpr int kResetSignals[] = {SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGQUIT};

  const pid_t parent_pid = getpid();
  const int stdin_fd = stdin_src.get();
  const int out_fd = out_w.get();
  const int err_fd = err_w.get();
  const int go_fd = go_r.get();
  const int execerr_fd = execerr_w.get();

  pid_t pid = fork();
  if (pid < 0) return fail("fork");

  if (pid == 0) {
    if (setsid() < 0) child_fail(execerr_fd, kSetsid, 125);
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0) child_fail(execerr_fd, kPrctl, 125);
    if (getppid() != parent_pid) _exit(125);  // parent already died
    if (sigprocmask(SIG_SETMASK, &empty_mask, nullptr) < 0) {
      child_fail(execerr_fd, kSigprocmask, 125);
    }
    for (int sig : kResetSignals) {
      if (sigaction(sig, &dfl, nullptr) < 0) child_fail(execerr_fd, kSigaction, 125);
    }
    if (stdin_fd >= 0 && dup2(stdin_fd, 0) < 0) child_fail(execerr_fd, kDup2, 125);
    if (dup2(out_fd, 1) < 0) child_fail(execerr_fd, kDup2, 125);
    if (dup2(err_fd, 2) < 0) child_fail(execerr_fd, kDup2, 125);
    if (close_range(3, ~0U, CLOSE_RANGE_CLOEXEC) < 0) child_fail(execerr_fd, kCloseRange, 125);
    char byte;
    ssize_t n;
    do {
      n = read(go_fd, &byte, 1);
    } while (n < 0 && errno == EINTR);
    if (n != 1) _exit(125);
    execvp(cargv[0], cargv.data());
    child_fail(execerr_fd, kExecvp, errno == ENOENT ? 127 : 126);
  }

  // The child is blocked on `go` and unreaped until the write below, so this
  // pid is ours to kill if setup fails.
  auto abort_child = [&s, pid](std::string error) {
    s.error = std::move(error);
    kill(pid, SIGKILL);
    reap_now(pid);
    s.pidfd.reset();
    s.pid = -1;
    return std::move(s);
  };

  s.pid = pid;
  s.pidfd.reset(pidfd_open(pid, 0));
  if (!s.pidfd) return abort_child(sys_error("pidfd_open", errno));

  out_w.reset();
  err_w.reset();
  go_r.reset();
  execerr_w.reset();
  stdin_src.reset();

  // Before `go`: the child cannot have forked yet, so every descendant is born
  // inside the box.
  if (box_procs_fd >= 0) {
    const std::string pid_text = std::to_string(pid);
    ssize_t n;
    do {
      n = write(box_procs_fd, pid_text.data(), pid_text.size());
    } while (n < 0 && errno == EINTR);
    if (n != static_cast<ssize_t>(pid_text.size())) {
      s.box_attach_failed = true;
      return abort_child(sys_error("write cgroup.procs", n < 0 ? errno : EIO));
    }
  }

  // EPIPE means the child already exited; execerr and the pidfd still say why.
  ssize_t w;
  do {
    w = write(go_w.get(), "x", 1);
  } while (w < 0 && errno == EINTR);
  go_w.reset();

  // EOF means execvp succeeded: O_CLOEXEC closed the child's write end. The
  // report is atomic, because it is smaller than PIPE_BUF.
  int report[2] = {0, 0};
  ssize_t r;
  do {
    r = read(execerr_r.get(), report, sizeof report);
  } while (r < 0 && errno == EINTR);
  if (r == static_cast<ssize_t>(sizeof report)) {
    const int steps = static_cast<int>(sizeof kChildSteps / sizeof kChildSteps[0]);
    s.exec_step = report[0] >= 0 && report[0] < steps ? kChildSteps[report[0]] : "child setup";
    s.exec_errno = report[1];
  }

  s.out_r = std::move(out_r);
  s.err_r = std::move(err_r);
  s.ok = true;
  return s;
}

}  // namespace corral
