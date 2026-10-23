#pragma once
// The systemd service notification protocol (sd_notify(3)) without libsystemd: one datagram to the
// AF_UNIX socket in $NOTIFY_SOCKET. fastmm-live and fastmm-gateway say READY=1 once they serve
// (Type=notify units start their dependents then, not at exec), WATCHDOG=1 from their control loop
// while $WATCHDOG_USEC asks for it (WatchdogSec=), and STOPPING=1 when their shutdown begins.
// Outside systemd ($NOTIFY_SOCKET unset) every call does nothing.
#include <cstdint>
#include <string_view>

namespace fastmm::live {

class SystemdNotifier {
 public:
  // Reads $NOTIFY_SOCKET, and $WATCHDOG_USEC when $WATCHDOG_PID is unset or this process's pid.
  SystemdNotifier() noexcept;
  ~SystemdNotifier();
  SystemdNotifier(const SystemdNotifier&) = delete;
  SystemdNotifier& operator=(const SystemdNotifier&) = delete;

  // Sends `state` ("READY=1\nSTATUS=trading"); false outside systemd or when the send failed.
  bool notify(std::string_view state) noexcept;
  // WATCHDOG=1 when half the watchdog interval has passed since the last one; `now_ns` is a
  // monotonic clock. Nothing without WatchdogSec=.
  void keep_alive(std::int64_t now_ns) noexcept;

  [[nodiscard]] bool active() const noexcept { return fd_ >= 0; }
  [[nodiscard]] std::int64_t watchdog_ns() const noexcept { return watchdog_ns_; }

 private:
  int fd_ = -1;
  std::int64_t watchdog_ns_ = 0;  // 0: no watchdog
  std::int64_t next_ping_ns_ = 0;
};

}  // namespace fastmm::live
