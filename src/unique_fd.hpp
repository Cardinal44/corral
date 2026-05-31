#pragma once

#include <unistd.h>

#include <utility>

namespace corral {

// Move-only owner of one file descriptor.
class unique_fd {
 public:
  unique_fd() noexcept = default;
  explicit unique_fd(int fd) noexcept : fd_(fd) {}
  ~unique_fd() { reset(); }

  unique_fd(const unique_fd&) = delete;
  unique_fd& operator=(const unique_fd&) = delete;

  unique_fd(unique_fd&& other) noexcept : fd_(other.release()) {}
  unique_fd& operator=(unique_fd&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }

  int get() const noexcept { return fd_; }
  bool valid() const noexcept { return fd_ >= 0; }
  explicit operator bool() const noexcept { return valid(); }

  int release() noexcept { return std::exchange(fd_, -1); }

  // close() errors are ignored: the fd is released either way on Linux, and
  // retrying after EINTR could close an fd another part of the program reused.
  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) ::close(fd_);
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

}  // namespace corral
