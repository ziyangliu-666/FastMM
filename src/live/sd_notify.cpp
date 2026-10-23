#include "fastmm/live/sd_notify.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace fastmm::live {

namespace {

// A positive decimal, 0 for anything else.
std::int64_t env_int(const char* name) noexcept {
  const char* v = std::getenv(name);
  if (v == nullptr || *v == '\0') return 0;
  char* end = nullptr;
  const long long n = std::strtoll(v, &end, 10);
  return end != nullptr && *end == '\0' && n > 0 ? static_cast<std::int64_t>(n) : 0;
}

}  // namespace

SystemdNotifier::SystemdNotifier() noexcept {
  const char* path = std::getenv("NOTIFY_SOCKET");
  if (path == nullptr || (path[0] != '/' && path[0] != '@')) return;
  const std::size_t len = std::strlen(path);
  sockaddr_un addr{};
  if (len >= sizeof addr.sun_path) return;
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path, len);
  if (path[0] == '@') addr.sun_path[0] = '\0';  // the abstract namespace
  fd_ = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd_ < 0) return;
  const auto addr_len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + len);
  if (::connect(fd_, reinterpret_cast<const sockaddr*>(&addr), addr_len) != 0) {
    ::close(fd_);
    fd_ = -1;
    return;
  }
  const std::int64_t pid = env_int("WATCHDOG_PID");
  if (pid == 0 || pid == static_cast<std::int64_t>(::getpid()))
    watchdog_ns_ = env_int("WATCHDOG_USEC") * 1000;
}

SystemdNotifier::~SystemdNotifier() {
  if (fd_ >= 0) ::close(fd_);
}

bool SystemdNotifier::notify(std::string_view state) noexcept {
  if (fd_ < 0) return false;
  return ::send(fd_, state.data(), state.size(), MSG_NOSIGNAL) ==
         static_cast<ssize_t>(state.size());
}

void SystemdNotifier::keep_alive(std::int64_t now_ns) noexcept {
  if (watchdog_ns_ <= 0 || now_ns < next_ping_ns_) return;
  next_ping_ns_ = now_ns + watchdog_ns_ / 2;
  static_cast<void>(notify("WATCHDOG=1"));
}

}  // namespace fastmm::live
