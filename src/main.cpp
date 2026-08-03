#include <fcntl.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "audit.hpp"
#include "cli.hpp"
#include "runner.hpp"
#include "unique_fd.hpp"

namespace {

int fail(const char* what) {
  std::fprintf(stderr, "corral: %s: %s\n", what, std::strerror(errno));
  return 125;
}

// Returns the failed call ("write" or "close") with errno set, or nullptr.
const char* write_and_close(corral::unique_fd fd, const std::string& data) {
  size_t done = 0;
  while (done < data.size()) {
    const ssize_t n = write(fd.get(), data.data() + done, data.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) return "write";
    done += static_cast<size_t>(n);
  }
  // Some filesystems (NFS) report a failed write only at close.
  return close(fd.release()) == 0 ? nullptr : "close";
}

}  // namespace

int main(int argc, char** argv) {
  // A closed stdout must surface as EPIPE from write(), not kill corral.
  // SIGCHLD must stay SIG_DFL: SIG_IGN makes the kernel auto-reap children.
  struct sigaction ign;
  std::memset(&ign, 0, sizeof ign);
  ign.sa_handler = SIG_IGN;
  sigemptyset(&ign.sa_mask);
  if (sigaction(SIGPIPE, &ign, nullptr) < 0) return fail("sigaction(SIGPIPE)");

  std::vector<std::string> args(argv + (argc > 0 ? 1 : 0), argv + argc);
  corral::CliParse parsed = corral::parse_args(args);
  if (!parsed.options) {
    std::fprintf(stderr, "corral: %s\n%s", parsed.error.c_str(), corral::kUsage);
    return 125;
  }
  const corral::CliOptions& opts = *parsed.options;
  if (opts.help) {
    std::fputs(corral::kUsage, stdout);
    return 0;
  }
  if (opts.version) {
    std::printf("corral %s\n", CORRAL_VERSION);
    return 0;
  }

  // Opened before the run, so a bad path fails before the command starts.
  corral::unique_fd json_fd;
  if (opts.json_path) {
    json_fd.reset(open(opts.json_path->c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666));
    if (!json_fd) {
      std::fprintf(stderr, "corral: open --json path '%s': %s\n", opts.json_path->c_str(),
                   std::strerror(errno));
      return 125;
    }
  }

  // Cancellation reaches run() as a readable fd instead of a signal handler.
  sigset_t cancel_signals;
  // These fail only for an invalid signal number, and the numbers are constants.
  sigemptyset(&cancel_signals);
  sigaddset(&cancel_signals, SIGINT);
  sigaddset(&cancel_signals, SIGTERM);
  sigaddset(&cancel_signals, SIGHUP);
  if (sigprocmask(SIG_BLOCK, &cancel_signals, nullptr) < 0) return fail("sigprocmask");
  corral::unique_fd sigfd(signalfd(-1, &cancel_signals, SFD_CLOEXEC | SFD_NONBLOCK));
  if (!sigfd) return fail("signalfd");

  corral::RunSpec spec = opts.spec;
  spec.cancel_fd = sigfd.get();
  corral::RunResult result = corral::run(spec);
  if (result.cancelled) {
    signalfd_siginfo info;
    if (read(sigfd.get(), &info, sizeof info) == static_cast<ssize_t>(sizeof info)) {
      result.cancel_signal = static_cast<int>(info.ssi_signo);
      result.corral_exit_code = corral::exit_code_for(result);
    }
  }
  for (const std::string& warning : result.warnings) {
    std::fprintf(stderr, "corral: warning: %s\n", warning.c_str());
  }
  if (!result.error.empty()) std::fprintf(stderr, "corral: %s\n", result.error.c_str());
  if (json_fd) {
    if (const char* failed = write_and_close(std::move(json_fd), corral::audit_json(result))) {
      std::fprintf(stderr, "corral: %s --json path '%s': %s\n", failed,
                   opts.json_path->c_str(), std::strerror(errno));
    }
  }
  if (!opts.quiet) std::fputs(corral::summary_line(result).c_str(), stderr);
  return result.corral_exit_code;
}
