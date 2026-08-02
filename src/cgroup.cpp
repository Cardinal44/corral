#include "cgroup.hpp"

#include <fcntl.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstring>
#include <initializer_list>

#include "procscan.hpp"
#include "unique_fd.hpp"

namespace corral {
namespace {

constexpr char kRoot[] = "/sys/fs/cgroup";

// Returns false with errno set.
bool write_file(const std::string& path, std::string_view data) {
  unique_fd fd(open(path.c_str(), O_WRONLY | O_CLOEXEC));
  if (!fd) return false;
  ssize_t n;
  do {
    n = write(fd.get(), data.data(), data.size());
  } while (n < 0 && errno == EINTR);
  if (n < 0) return false;
  // cgroupfs takes each write() as one command, so a short write is a failure.
  if (static_cast<size_t>(n) != data.size()) {
    errno = EIO;
    return false;
  }
  return true;
}

std::optional<uint64_t> parse_number(std::string_view s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.remove_suffix(1);
  uint64_t value;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
  if (s.empty() || ec != std::errc() || ptr != s.data() + s.size()) return std::nullopt;
  return value;
}

bool has_word(std::string_view list, std::string_view word) {
  while (!list.empty()) {
    const size_t start = list.find_first_not_of(" \n");
    if (start == std::string_view::npos) break;
    list.remove_prefix(start);
    const size_t end = list.find_first_of(" \n");
    if (list.substr(0, end) == word) return true;
    if (end == std::string_view::npos) break;
    list.remove_prefix(end);
  }
  return false;
}

std::string parent_of(const std::string& path) { return path.substr(0, path.rfind('/')); }
std::string name_of(const std::string& path) { return path.substr(path.rfind('/') + 1); }

// Call right after the failing syscall, before anything can change errno.
std::string failed(const std::string& step) { return step + ": " + std::strerror(errno); }

// A cgroup that contains processes cannot enable controllers for its children,
// so this process first moves into G/corral-sup. That happens once per process:
// afterwards its own cgroup is corral-sup, and the box parent is G.
std::optional<std::string> delegated_parent(BoxSetup& s) {
  auto file = read_file("/proc/self/cgroup");
  auto rel = file ? parse_proc_cgroup(*file) : std::nullopt;
  if (!rel) {
    s.reason = "no_cgroup2";
    s.error = "/proc/self/cgroup has no cgroup v2 entry";
    return std::nullopt;
  }
  const std::string g = kRoot + (*rel == "/" ? std::string() : *rel);
  if (name_of(g) == "corral-sup") return parent_of(g);

  auto not_delegated = [&s](std::string error) {
    s.reason = "cgroup_not_delegated";
    s.error = std::move(error);
    return std::nullopt;
  };
  for (const std::string& path : {g, g + "/cgroup.procs", g + "/cgroup.subtree_control"}) {
    if (access(path.c_str(), W_OK) < 0) return not_delegated(failed("access " + path));
  }
  const std::string leaf = g + "/corral-sup";
  if (mkdir(leaf.c_str(), 0755) < 0 && errno != EEXIST) {
    return not_delegated(failed("mkdir " + leaf));
  }
  if (!write_file(leaf + "/cgroup.procs", std::to_string(getpid()))) {
    return not_delegated(failed("write " + leaf + "/cgroup.procs"));
  }
  const std::string controllers = read_file(g + "/cgroup.controllers").value_or("");
  for (const char* controller : {"memory", "pids"}) {
    if (!has_word(controllers, controller)) continue;
    const std::string command = std::string("+") + controller;
    if (!write_file(g + "/cgroup.subtree_control", command)) {
      // EBUSY: other processes still share G, so it cannot have child controllers.
      s.reason = errno == EBUSY ? "delegated_cgroup_not_empty" : "cgroup_not_delegated";
      s.error = failed("write " + command + " to " + g + "/cgroup.subtree_control");
      return std::nullopt;
    }
  }
  return g;
}

enum class Applied { kOk, kMissing, kFailed };

// kMissing: the controller is not enabled for the box, so its file is absent.
Applied apply(const std::string& file, const std::string& value, std::string& error) {
  if (write_file(file, value)) return Applied::kOk;
  if (errno == ENOENT) return Applied::kMissing;
  error = failed("write " + value + " to " + file);
  return Applied::kFailed;
}

}  // namespace

std::optional<uint64_t> parse_keyed_value(std::string_view text, std::string_view key) {
  while (!text.empty()) {
    const size_t end = text.find('\n');
    const std::string_view line = text.substr(0, end);
    const size_t space = line.find(' ');
    if (space != std::string_view::npos && line.substr(0, space) == key) {
      return parse_number(line.substr(space + 1));
    }
    if (end == std::string_view::npos) break;
    text.remove_prefix(end + 1);
  }
  return std::nullopt;
}

std::string Box::relative_path() const { return path_.substr(sizeof kRoot - 1); }

std::optional<std::vector<pid_t>> Box::live_members() const {
  auto text = read_file(path_ + "/cgroup.procs");
  if (!text) return std::nullopt;
  std::vector<pid_t> live;
  std::string_view rest = *text;
  while (!rest.empty()) {
    const size_t end = rest.find('\n');
    const std::string_view line = rest.substr(0, end);
    pid_t pid;
    auto [ptr, ec] = std::from_chars(line.data(), line.data() + line.size(), pid);
    if (ec == std::errc() && ptr == line.data() + line.size()) {
      auto st = read_proc_stat(pid);
      if (st && is_live(*st)) live.push_back(pid);
    }
    if (end == std::string_view::npos) break;
    rest.remove_prefix(end + 1);
  }
  return live;
}

bool Box::populated() const {
  auto text = read_file(path_ + "/cgroup.events");
  auto value = text ? parse_keyed_value(*text, "populated") : std::nullopt;
  return !value || *value != 0;
}

void Box::kill() const {
  // Errors are ignored like signal errors: verification reports what survives.
  write_file(path_ + "/cgroup.kill", "1");
}

LimitEvents Box::events() const {
  LimitEvents ev;
  if (auto text = read_file(path_ + "/memory.events")) {
    ev.oom_kills = parse_keyed_value(*text, "oom_kill");
  }
  if (auto text = read_file(path_ + "/pids.events")) {
    ev.pids_max_hits = parse_keyed_value(*text, "max");
  }
  if (auto text = read_file(path_ + "/memory.peak")) ev.peak_mem_bytes = parse_number(*text);
  return ev;
}

bool Box::remove(TimePoint deadline, std::string& error) const {
  for (;;) {
    if (rmdir(path_.c_str()) == 0) return true;
    if (errno != EBUSY || now() >= deadline) {
      error = failed("rmdir " + path_);
      return false;
    }
    sleep_ms(10);
  }
}

BoxSetup create_box(const RunSpec& spec) {
  BoxSetup s;
  struct statfs fs;
  if (statfs(kRoot, &fs) < 0 || fs.f_type != CGROUP2_SUPER_MAGIC) {
    s.reason = "no_cgroup2";
    s.error = std::string(kRoot) + " is not a cgroup v2 mount";
    return s;
  }

  std::string parent;
  if (spec.cgroup_parent) {
    parent = *spec.cgroup_parent;
    while (parent.size() > 1 && parent.back() == '/') parent.pop_back();
    if (parent.compare(0, sizeof kRoot - 1, kRoot) != 0 || parent.size() == sizeof kRoot - 1) {
      s.reason = "cgroup_not_delegated";
      s.error = "--cgroup-parent must be a cgroup below " + std::string(kRoot) + ": " + parent;
      return s;
    }
    if (access(parent.c_str(), W_OK) < 0) {
      s.reason = "cgroup_not_delegated";
      s.error = failed("access " + parent);
      return s;
    }
  } else {
    auto delegated = delegated_parent(s);
    if (!delegated) return s;
    parent = *delegated;
  }

  static unsigned counter = 0;
  const std::string path =
      parent + "/corral-run-" + std::to_string(getpid()) + "-" + std::to_string(++counter);
  if (mkdir(path.c_str(), 0755) < 0) {
    s.reason = "cgroup_setup_error";
    s.error = failed("mkdir " + path);
    return s;
  }
  auto abandon = [&path](const char* reason, std::string error) {
    rmdir(path.c_str());  // best effort: the setup already failed with `error`
    BoxSetup out;
    out.reason = reason;
    out.error = std::move(error);
    return out;
  };
  if (access((path + "/cgroup.kill").c_str(), F_OK) < 0) {
    return abandon("cgroup_kill_unsupported", path + "/cgroup.kill does not exist");
  }

  std::string error;
  if (spec.mem_bytes) {
    Applied a = apply(path + "/memory.max", std::to_string(*spec.mem_bytes), error);
    // Without this, the kernel would push the excess to swap instead of OOM-killing.
    if (a == Applied::kOk && access((path + "/memory.swap.max").c_str(), F_OK) == 0) {
      a = apply(path + "/memory.swap.max", "0", error);
    }
    // One OOM kill takes the whole box, not just the largest process.
    if (a == Applied::kOk) a = apply(path + "/memory.oom.group", "1", error);
    if (a == Applied::kFailed) return abandon("cgroup_setup_error", error);
    s.mem_applied = a == Applied::kOk;
    if (a == Applied::kMissing) {
      s.warnings.push_back("no memory controller in " + path + ": --mem is not enforced");
    }
  }
  if (spec.pids) {
    const Applied a = apply(path + "/pids.max", std::to_string(*spec.pids), error);
    if (a == Applied::kFailed) return abandon("cgroup_setup_error", error);
    s.pids_applied = a == Applied::kOk;
    if (a == Applied::kMissing) {
      s.warnings.push_back("no pids controller in " + path + ": --pids is not enforced");
    }
  }
  s.box.emplace(path);
  return s;
}

}  // namespace corral
