#include "procscan.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
extern "C" {  // see spawn.cpp
#include <sys/pidfd.h>
}
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <memory>

#include "unique_fd.hpp"

namespace corral {
namespace {

std::string_view next_field(std::string_view& rest) {
  size_t start = rest.find_first_not_of(' ');
  if (start == std::string_view::npos) {
    rest = {};
    return {};
  }
  rest.remove_prefix(start);
  size_t end = rest.find(' ');
  std::string_view field = rest.substr(0, end);
  rest.remove_prefix(end == std::string_view::npos ? rest.size() : end);
  return field;
}

template <typename T>
bool parse_int(std::string_view s, T& out) {
  if (s.empty()) return false;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc() && ptr == s.data() + s.size();
}

std::optional<std::string> read_proc_file(pid_t pid, const char* name) {
  return read_file("/proc/" + std::to_string(pid) + "/" + name);
}

}  // namespace

std::optional<std::string> read_file(const std::string& path) {
  unique_fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (!fd) return std::nullopt;
  std::string out;
  char buf[4096];
  for (;;) {
    const ssize_t n = read(fd.get(), buf, sizeof buf);
    if (n > 0) {
      out.append(buf, static_cast<size_t>(n));
    } else if (n == 0) {
      return out;
    } else if (errno != EINTR) {
      return std::nullopt;  // ESRCH: the process exited after open
    }
  }
}

std::optional<ProcStat> parse_proc_stat(std::string_view line) {
  const size_t open = line.find('(');
  const size_t close = line.rfind(')');
  if (open == std::string_view::npos || close == std::string_view::npos || close < open)
    return std::nullopt;

  ProcStat st;
  st.comm = std::string(line.substr(open + 1, close - open - 1));
  std::string_view rest = line.substr(close + 1);
  const std::string_view state = next_field(rest);
  if (state.size() != 1) return std::nullopt;
  st.state = state[0];
  if (!parse_int(next_field(rest), st.ppid) || !parse_int(next_field(rest), st.pgrp) ||
      !parse_int(next_field(rest), st.session))
    return std::nullopt;
  return st;
}

std::optional<ProcStat> read_proc_stat(pid_t pid) {
  auto text = read_proc_file(pid, "stat");
  if (!text) return std::nullopt;
  return parse_proc_stat(*text);
}

std::optional<std::string> parse_proc_cgroup(std::string_view file) {
  while (!file.empty()) {
    const size_t end = file.find('\n');
    const std::string_view line = file.substr(0, end);
    if (line.substr(0, 3) == "0::") return std::string(line.substr(3));
    if (end == std::string_view::npos) break;
    file.remove_prefix(end + 1);
  }
  return std::nullopt;
}

std::optional<std::string> read_proc_cgroup(pid_t pid) {
  auto text = read_proc_file(pid, "cgroup");
  if (!text) return std::nullopt;
  return parse_proc_cgroup(*text);
}

std::vector<pid_t> list_procs() {
  std::vector<pid_t> pids;
  unique_fd fd(open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!fd) return pids;
  std::unique_ptr<DIR, int (*)(DIR*)> dir(fdopendir(fd.get()), closedir);
  if (!dir) return pids;
  fd.release();  // now owned by `dir`
  while (dirent* e = readdir(dir.get())) {
    pid_t pid;
    std::string_view name(e->d_name);
    if (parse_int(name, pid)) pids.push_back(pid);
  }
  return pids;
}

bool is_live(const ProcStat& st) { return st.state != 'Z' && st.state != 'X'; }

bool RunTarget::in_fallback_set(pid_t pid, const ProcStat& st) const {
  return pid != self && (st.ppid == self || st.pgrp == child || st.session == child);
}

bool RunTarget::matches(pid_t pid, const ProcStat& st) const {
  if (!is_live(st)) return false;
  if (in_fallback_set(pid, st)) return true;
  return !box.empty() && read_proc_cgroup(pid) == box;
}

std::vector<pid_t> fallback_live_set(const RunTarget& target) {
  std::vector<pid_t> live;
  for (pid_t pid : list_procs()) {
    auto st = read_proc_stat(pid);
    if (st && is_live(*st) && target.in_fallback_set(pid, *st)) live.push_back(pid);
  }
  return live;
}

void safe_signal(pid_t pid, int sig, const RunTarget& target) {
  auto st = read_proc_stat(pid);
  if (!st || !is_live(*st)) return;
  // An unreaped child's pid cannot be recycled, and only corral reaps it.
  if (st->ppid == target.self) {
    kill(pid, sig);
    return;
  }
  // Anyone else's pid can be recycled between the scan and the signal. Bind a
  // pidfd first, then re-check that the process behind the pid still qualifies.
  unique_fd pidfd(pidfd_open(pid, 0));
  if (!pidfd) return;
  st = read_proc_stat(pid);
  if (!st || !target.matches(pid, *st)) return;
  pidfd_send_signal(pidfd.get(), sig, nullptr, 0);
}

}  // namespace corral
