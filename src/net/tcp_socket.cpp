#include "fastmm/net/tcp_socket.hpp"

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace fastmm::net {

// ---------------------------------------------------------------------------- SockAddr

std::optional<SockAddr> SockAddr::from_ip(std::string_view ip, std::uint16_t port) noexcept {
  char buf[INET6_ADDRSTRLEN];
  if (ip.size() >= sizeof(buf)) return std::nullopt;
  std::memcpy(buf, ip.data(), ip.size());
  buf[ip.size()] = '\0';
  SockAddr a;
  auto* v4 = reinterpret_cast<sockaddr_in*>(&a.storage);
  if (::inet_pton(AF_INET, buf, &v4->sin_addr) == 1) {
    v4->sin_family = AF_INET;
    v4->sin_port = htons(port);
    a.len = sizeof(sockaddr_in);
    return a;
  }
  auto* v6 = reinterpret_cast<sockaddr_in6*>(&a.storage);
  if (::inet_pton(AF_INET6, buf, &v6->sin6_addr) == 1) {
    v6->sin6_family = AF_INET6;
    v6->sin6_port = htons(port);
    a.len = sizeof(sockaddr_in6);
    return a;
  }
  return std::nullopt;
}

SockAddr SockAddr::loopback_v4(std::uint16_t port) noexcept {
  SockAddr a;
  auto* v4 = reinterpret_cast<sockaddr_in*>(&a.storage);
  v4->sin_family = AF_INET;
  v4->sin_port = htons(port);
  v4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.len = sizeof(sockaddr_in);
  return a;
}

SockAddr SockAddr::any_v4(std::uint16_t port) noexcept {
  SockAddr a = loopback_v4(port);
  reinterpret_cast<sockaddr_in*>(&a.storage)->sin_addr.s_addr = htonl(INADDR_ANY);
  return a;
}

std::uint16_t SockAddr::port() const noexcept {
  if (storage.ss_family == AF_INET) {
    return ntohs(reinterpret_cast<const sockaddr_in*>(&storage)->sin_port);
  }
  if (storage.ss_family == AF_INET6) {
    return ntohs(reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_port);
  }
  return 0;
}

std::size_t SockAddr::format(std::span<char> out) const noexcept {
  char ip[INET6_ADDRSTRLEN] = {};
  const bool v6 = storage.ss_family == AF_INET6;
  const void* src =
      v6 ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_addr)
         : static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(&storage)->sin_addr);
  if (storage.ss_family != AF_INET && !v6) return 0;
  if (::inet_ntop(storage.ss_family, src, ip, sizeof(ip)) == nullptr) return 0;
  const int n = v6 ? std::snprintf(out.data(), out.size(), "[%s]:%u", ip, port())
                   : std::snprintf(out.data(), out.size(), "%s:%u", ip, port());
  if (n < 0 || static_cast<std::size_t>(n) >= out.size()) return 0;
  return static_cast<std::size_t>(n);
}

std::string SockAddr::to_string() const {
  char buf[64];
  return std::string(buf, format(buf));
}

// --------------------------------------------------------------------------- TcpSocket

TcpSocket TcpSocket::open(int family) noexcept {
  return TcpSocket(::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
}

bool TcpSocket::make_pair(TcpSocket& a, TcpSocket& b) noexcept {
  int fds[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) != 0) return false;
  a = TcpSocket(fds[0]);
  b = TcpSocket(fds[1]);
  return true;
}

TcpSocket TcpSocket::listen(const SockAddr& bind_addr, int backlog) noexcept {
  TcpSocket s = open(bind_addr.family());
  if (!s.valid()) return s;
  s.set_int_opt(SOL_SOCKET, SO_REUSEADDR, 1);
  if (::bind(s.fd_, bind_addr.ptr(), bind_addr.len) != 0 || ::listen(s.fd_, backlog) != 0) {
    const int e = errno;
    s.close();
    s.err_ = e;
  }
  return s;
}

ConnectStatus TcpSocket::connect(const SockAddr& addr) noexcept {
  if (!valid()) {
    *this = open(addr.family());
    if (!valid()) {
      err_ = errno;
      return ConnectStatus::Error;
    }
  }
  if (::connect(fd_, addr.ptr(), addr.len) == 0) {
    connecting_ = false;
    return ConnectStatus::Connected;  // loopback may complete synchronously
  }
  if (errno == EINPROGRESS) {
    connecting_ = true;
    return ConnectStatus::InProgress;
  }
  err_ = errno;
  return ConnectStatus::Error;
}

int TcpSocket::finish_connect() noexcept {
  connecting_ = false;
  err_ = pending_error();
  return err_;
}

int TcpSocket::pending_error() const noexcept {
  int err = 0;
  socklen_t len = sizeof(err);
  if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) != 0) return errno;
  return err;
}

TcpSocket TcpSocket::accept(SockAddr* peer) noexcept {
  sockaddr_storage ss{};
  socklen_t len = sizeof(ss);
  const int fd =
      ::accept4(fd_, reinterpret_cast<sockaddr*>(&ss), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (fd < 0) {
    err_ = errno == EAGAIN ? 0 : errno;
    return TcpSocket{};
  }
  if (peer != nullptr) {
    peer->storage = ss;
    peer->len = len;
  }
  return TcpSocket(fd);
}

IoResult TcpSocket::read(std::span<std::byte> buf) noexcept {
  input_drained_ = false;
  if (buf.empty()) return IoResult::done(0);
  for (;;) {
    const ssize_t n = ::recv(fd_, buf.data(), buf.size(), 0);
    if (n > 0) {
      input_drained_ = static_cast<std::size_t>(n) < buf.size();
      return IoResult::done(static_cast<std::size_t>(n));
    }
    if (n == 0) return IoResult::eof();
    if (errno == EINTR) continue;
    if (errno == EAGAIN) return IoResult::wants_read();  // == EWOULDBLOCK on Linux
    err_ = errno;
    return IoResult::syscall(errno);
  }
}

IoResult TcpSocket::write(std::span<const std::byte> buf) noexcept {
  std::size_t total = 0;
  while (total < buf.size()) {
    const ssize_t n = ::send(fd_, buf.data() + total, buf.size() - total, MSG_NOSIGNAL);
    if (n > 0) {
      total += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && errno == EAGAIN) return IoResult::wants_write(total);
    if (n < 0 && errno == EPIPE) {
      IoResult r = IoResult::eof();
      r.bytes = total;
      return r;
    }
    err_ = errno;
    IoResult r = IoResult::syscall(errno);
    r.bytes = total;
    return r;
  }
  return IoResult::done(total);
}

bool TcpSocket::set_int_opt(int level, int name, int value) noexcept {
  if (::setsockopt(fd_, level, name, &value, sizeof(value)) != 0) {
    err_ = errno;
    return false;
  }
  return true;
}

bool TcpSocket::set_nodelay(bool on) noexcept {
  return set_int_opt(IPPROTO_TCP, TCP_NODELAY, on ? 1 : 0);
}
bool TcpSocket::set_keepalive(bool on) noexcept {
  return set_int_opt(SOL_SOCKET, SO_KEEPALIVE, on ? 1 : 0);
}
bool TcpSocket::set_rcvbuf(int bytes) noexcept {
  return set_int_opt(SOL_SOCKET, SO_RCVBUF, bytes);
}
bool TcpSocket::set_sndbuf(int bytes) noexcept {
  return set_int_opt(SOL_SOCKET, SO_SNDBUF, bytes);
}

std::optional<SockAddr> TcpSocket::local_addr() const noexcept {
  SockAddr a;
  a.len = sizeof(a.storage);
  if (::getsockname(fd_, a.ptr(), &a.len) != 0) return std::nullopt;
  return a;
}

std::optional<SockAddr> TcpSocket::peer_addr() const noexcept {
  SockAddr a;
  a.len = sizeof(a.storage);
  if (::getpeername(fd_, a.ptr(), &a.len) != 0) return std::nullopt;
  return a;
}

void TcpSocket::shutdown_write() noexcept {
  if (valid()) ::shutdown(fd_, SHUT_WR);
}

void TcpSocket::close() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  connecting_ = false;
}

}  // namespace fastmm::net
