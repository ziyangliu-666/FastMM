#pragma once
// Non-blocking TCP socket with RAII fd ownership. All I/O is non-throwing and returns IoResult.
#include "fastmm/net/io_result.hpp"

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace fastmm::net {

// IPv4/IPv6 socket address (value type, no allocation).
struct SockAddr {
  sockaddr_storage storage{};
  socklen_t len = 0;

  static std::optional<SockAddr> from_ip(std::string_view ip, std::uint16_t port) noexcept;
  static SockAddr loopback_v4(std::uint16_t port) noexcept;
  static SockAddr any_v4(std::uint16_t port) noexcept;

  int family() const noexcept { return storage.ss_family; }
  std::uint16_t port() const noexcept;
  const sockaddr* ptr() const noexcept { return reinterpret_cast<const sockaddr*>(&storage); }
  sockaddr* ptr() noexcept { return reinterpret_cast<sockaddr*>(&storage); }
  bool valid() const noexcept { return len != 0; }

  // "ip:port" ("[ip]:port" for IPv6). Returns bytes written, 0 if `out` is too small.
  std::size_t format(std::span<char> out) const noexcept;
  std::string to_string() const;
};

enum class ConnectStatus : std::uint8_t { Connected, InProgress, Error };

class TcpSocket {
 public:
  TcpSocket() = default;
  explicit TcpSocket(int fd) noexcept : fd_(fd) {}
  ~TcpSocket() { close(); }
  TcpSocket(const TcpSocket&) = delete;
  TcpSocket& operator=(const TcpSocket&) = delete;
  TcpSocket(TcpSocket&& o) noexcept : fd_(o.fd_), connecting_(o.connecting_), err_(o.err_) {
    o.fd_ = -1;
    o.connecting_ = false;
  }
  TcpSocket& operator=(TcpSocket&& o) noexcept {
    if (this != &o) {
      close();
      fd_ = o.fd_;
      connecting_ = o.connecting_;
      err_ = o.err_;
      o.fd_ = -1;
      o.connecting_ = false;
    }
    return *this;
  }

  // Creates a non-blocking, close-on-exec stream socket of the given family.
  static TcpSocket open(int family) noexcept;
  // AF_UNIX stream pair (both non-blocking) for in-process tests.
  static bool make_pair(TcpSocket& a, TcpSocket& b) noexcept;
  // Bound + listening socket (SO_REUSEADDR). Invalid socket on failure (errno in last_error()).
  static TcpSocket listen(const SockAddr& bind_addr, int backlog = 128) noexcept;

  // Starts a non-blocking connect (opens the socket if needed). On InProgress, wait for
  // writability and call finish_connect().
  ConnectStatus connect(const SockAddr& addr) noexcept;
  // Completes an in-progress connect: returns 0 on success, otherwise the errno (SO_ERROR).
  int finish_connect() noexcept;
  // SO_ERROR without changing the connecting state (0 also while still in progress).
  int pending_error() const noexcept;
  bool connecting() const noexcept { return connecting_; }

  // Accepts one pending connection; returns an invalid socket when none is pending (EAGAIN)
  // or on error (see last_error()).
  TcpSocket accept(SockAddr* peer = nullptr) noexcept;

  IoResult read(std::span<std::byte> buf) noexcept;
  IoResult write(std::span<const std::byte> buf) noexcept;
  // The last read() returned fewer bytes than asked: the receive queue was empty after it. With
  // edge-triggered readiness the caller may skip the read that would only return EAGAIN, unless
  // the event also reported a hang-up (Reactor::event_hangup()).
  bool input_drained() const noexcept { return input_drained_; }

  bool set_nodelay(bool on) noexcept;
  bool set_keepalive(bool on) noexcept;
  bool set_rcvbuf(int bytes) noexcept;
  bool set_sndbuf(int bytes) noexcept;
  std::optional<SockAddr> local_addr() const noexcept;
  std::optional<SockAddr> peer_addr() const noexcept;

  void shutdown_write() noexcept;
  void close() noexcept;
  int release() noexcept {
    const int f = fd_;
    fd_ = -1;
    return f;
  }

  int fd() const noexcept { return fd_; }
  bool valid() const noexcept { return fd_ >= 0; }
  int last_error() const noexcept { return err_; }

 private:
  bool set_int_opt(int level, int name, int value) noexcept;

  int fd_ = -1;
  bool connecting_ = false;
  bool input_drained_ = false;
  int err_ = 0;
};

}  // namespace fastmm::net
