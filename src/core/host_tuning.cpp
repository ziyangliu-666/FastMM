#include "fastmm/core/host_tuning.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>

namespace fastmm {

int CpuLatencyRequest::open(std::int32_t max_us, const char* path) noexcept {
  close();
  if (max_us < 0) return EINVAL;
  const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) return errno;
  const ssize_t n = ::write(fd, &max_us, sizeof max_us);
  if (n != static_cast<ssize_t>(sizeof max_us)) {
    const int err = n < 0 ? errno : EIO;
    ::close(fd);
    return err;
  }
  fd_ = fd;
  return 0;
}

void CpuLatencyRequest::close() noexcept {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

}  // namespace fastmm
