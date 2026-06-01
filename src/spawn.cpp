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

[[noreturn]] void child_fail(int execerr_w, int code) {
  int err = errno;
  ssize_t n = write(execerr_w, &err, sizeof err);
  (void)n;
  _exit(code);
}

void reap_now(pid_t pid) {
  int status;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
}

}  // namespace

Spawned spawn_child(const std::vector<std::string>& argv, StdinPolicy stdin_policy) {
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

  // Prepared before fork so the child makes only the calls listed in 7.3.
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
    // Only: setsid prctl getppid sigprocmask sigaction dup2 close close_range
    // read write execvp _exit.
    if (setsid() < 0) child_fail(execerr_fd, 125);
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0) child_fail(execerr_fd, 125);
    if (getppid() != parent_pid) _exit(125);  // parent already died
    if (sigprocmask(SIG_SETMASK, &empty_mask, nullptr) < 0) child_fail(execerr_fd, 125);
    for (int sig : kResetSignals) {
      if (sigaction(sig, &dfl, nullptr) < 0) child_fail(execerr_fd, 125);
    }
    if (stdin_fd >= 0 && dup2(stdin_fd, 0) < 0) child_fail(execerr_fd, 125);
    if (dup2(out_fd, 1) < 0) child_fail(execerr_fd, 125);
    if (dup2(err_fd, 2) < 0) child_fail(execerr_fd, 125);
    if (close_range(3, ~0U, CLOSE_RANGE_CLOEXEC) < 0) child_fail(execerr_fd, 125);
    char byte;
    ssize_t n;
    do {
      n = read(go_fd, &byte, 1);
    } while (n < 0 && errno == EINTR);
    if (n != 1) _exit(125);
    execvp(cargv[0], cargv.data());
    child_fail(execerr_fd, errno == ENOENT ? 127 : 126);
  }

  s.pid = pid;
  s.pidfd.reset(pidfd_open(pid, 0));
  if (!s.pidfd) {
    s.error = sys_error("pidfd_open", errno);
    // The child is still blocked on `go` and unreaped, so this pid is ours.
    kill(pid, SIGKILL);
    reap_now(pid);
    s.pid = -1;
    return s;
  }

  out_w.reset();
  err_w.reset();
  go_r.reset();
  execerr_w.reset();
  stdin_src.reset();

  // EPIPE means the child already exited; execerr and the pidfd still say why.
  ssize_t w;
  do {
    w = write(go_w.get(), "x", 1);
  } while (w < 0 && errno == EINTR);
  go_w.reset();

  // EOF means execvp succeeded: O_CLOEXEC closed the child's write end. The
  // errno report is atomic (sizeof(int) <= PIPE_BUF).
  int child_errno = 0;
  ssize_t r;
  do {
    r = read(execerr_r.get(), &child_errno, sizeof child_errno);
  } while (r < 0 && errno == EINTR);
  if (r == static_cast<ssize_t>(sizeof child_errno)) s.exec_errno = child_errno;

  s.out_r = std::move(out_r);
  s.err_r = std::move(err_r);
  s.ok = true;
  return s;
}

}  // namespace corral
