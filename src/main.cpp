#include <signal.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cli.hpp"
#include "runner.hpp"

int main(int argc, char** argv) {
  // A closed stdout must surface as EPIPE from write(), not kill corral.
  // SIGCHLD must stay SIG_DFL: SIG_IGN makes the kernel auto-reap children.
  struct sigaction ign;
  std::memset(&ign, 0, sizeof ign);
  ign.sa_handler = SIG_IGN;
  sigemptyset(&ign.sa_mask);
  if (sigaction(SIGPIPE, &ign, nullptr) < 0) {
    std::fprintf(stderr, "corral: sigaction(SIGPIPE): %s\n", std::strerror(errno));
    return 125;
  }

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

  corral::RunResult result = corral::run(opts.spec);
  if (!result.error.empty()) std::fprintf(stderr, "corral: %s\n", result.error.c_str());
  return result.corral_exit_code;
}
