#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cli.hpp"
#include "runner.hpp"
#include "unique_fd.hpp"

namespace {

int fail(const char* what) {
  std::fprintf(stderr, "corral: %s: %s\n", what, std::strerror(errno));
  return 125;
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

  // Cancellation reaches run() as a readable fd instead of a signal handler.
  sigset_t cancel_signals;
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
  if (!result.error.empty()) std::fprintf(stderr, "corral: %s\n", result.error.c_str());
  return result.corral_exit_code;
}
