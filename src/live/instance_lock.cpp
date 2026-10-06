#include "fastmm/live/instance_lock.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>

namespace fastmm::live {

InstanceLock::Status InstanceLock::try_acquire(const std::string& path, std::string* error) {
  if (fd_ >= 0) return Status::Acquired;
  const auto fail = [&](std::string_view what) {
    if (error != nullptr) *error = std::string(what) + " " + path + ": " + std::strerror(errno);
    return Status::Error;
  };
  if (const std::filesystem::path p(path); p.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
  }
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return fail("cannot open the instance lock");
  int rc = 0;
  do {
    rc = ::flock(fd, LOCK_EX | LOCK_NB);
  } while (rc != 0 && errno == EINTR);
  if (rc != 0) {
    const int err = errno;
    ::close(fd);
    if (err == EWOULDBLOCK) return Status::Held;
    errno = err;
    return fail("cannot lock the instance lock");
  }
  fd_ = fd;
  path_ = path;
  // Diagnostics only: whoever waits names the holder.
  char buf[24];
  const auto [end, ec] = std::to_chars(buf, buf + sizeof buf - 1, ::getpid());
  *end = '\n';
  if (::ftruncate(fd_, 0) == 0)
    static_cast<void>(::pwrite(fd_, buf, static_cast<std::size_t>(end + 1 - buf), 0) > 0);
  return Status::Acquired;
}

void InstanceLock::release() noexcept {
  if (fd_ < 0) return;
  // The pid goes with the lock: a reader must not name a process that no longer holds it.
  static_cast<void>(::ftruncate(fd_, 0) == 0);
  static_cast<void>(::flock(fd_, LOCK_UN));
  ::close(fd_);
  fd_ = -1;
}

std::uint32_t InstanceLock::holder_pid(const std::string& path) noexcept {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  char buf[24] = {};
  const ssize_t n = ::pread(fd, buf, sizeof buf - 1, 0);
  ::close(fd);
  if (n <= 0) return 0;
  std::uint32_t pid = 0;
  const auto [ptr, ec] = std::from_chars(buf, buf + n, pid);
  return ec == std::errc{} ? pid : 0;
}

std::string instance_lock_path(const std::string& lock_file,
                               const std::string& journal_dir,
                               const std::string& engine_name) {
  if (!lock_file.empty()) return lock_file;
  return journal_dir + "/" + engine_name + ".lock";
}

}  // namespace fastmm::live
