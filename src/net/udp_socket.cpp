#include "fastmm/net/udp_socket.hpp"

#include <arpa/inet.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

// Old glibc / kernel headers (manylinux) lack the newer socket options.
#ifndef SO_BUSY_POLL
#define SO_BUSY_POLL 46  // NOLINT(readability-identifier-naming): Linux 3.11
#endif
#ifndef IP_MULTICAST_ALL
#define IP_MULTICAST_ALL 49  // NOLINT(readability-identifier-naming): Linux 2.6.31
#endif
#ifndef SO_PREFER_BUSY_POLL
#define SO_PREFER_BUSY_POLL 69  // NOLINT(readability-identifier-naming): Linux 5.11
#endif
#ifndef SO_BUSY_POLL_BUDGET
#define SO_BUSY_POLL_BUDGET 70  // NOLINT(readability-identifier-naming): Linux 5.11
#endif

namespace fastmm::net {

bool parse_ipv4(std::string_view text, std::uint32_t& out_be) noexcept {
  char buf[INET_ADDRSTRLEN];
  if (text.empty() || text.size() >= sizeof(buf)) return false;
  std::memcpy(buf, text.data(), text.size());
  buf[text.size()] = '\0';
  in_addr a{};
  if (::inet_pton(AF_INET, buf, &a) != 1) return false;
  out_be = a.s_addr;
  return true;
}

int McastInterface::resolve(std::string_view spec, McastInterface& out) noexcept {
  out = McastInterface{};
  if (spec.empty()) return 0;
  if (parse_ipv4(spec, out.addr_be)) return 0;
  char name[IF_NAMESIZE];
  if (spec.size() >= sizeof(name)) return -ENODEV;
  std::memcpy(name, spec.data(), spec.size());
  name[spec.size()] = '\0';
  out.index = ::if_nametoindex(name);
  return out.index == 0 ? -ENODEV : 0;
}

int enable_hw_timestamps(std::string_view ifname) noexcept {
  ifreq ifr{};
  if (ifname.empty() || ifname.size() >= sizeof(ifr.ifr_name)) return -ENODEV;
  std::memcpy(ifr.ifr_name, ifname.data(), ifname.size());
  hwtstamp_config cfg{};
  cfg.tx_type = HWTSTAMP_TX_OFF;
  cfg.rx_filter = HWTSTAMP_FILTER_ALL;
  ifr.ifr_data = reinterpret_cast<char*>(&cfg);
  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -errno;
  const int rc = ::ioctl(fd, SIOCSHWTSTAMP, &ifr) == 0 ? 0 : -errno;
  ::close(fd);
  return rc;
}

UdpSocket UdpSocket::open() noexcept {
  UdpSocket s(::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (!s.valid()) s.err_ = errno;
  return s;
}

int UdpSocket::fail() noexcept {
  err_ = errno;
  return -err_;
}

int UdpSocket::set_int_opt(int level, int name, int value) noexcept {
  if (::setsockopt(fd_, level, name, &value, sizeof(value)) != 0) return fail();
  return 0;
}

int UdpSocket::bind(const SockAddr& addr) noexcept {
  return ::bind(fd_, addr.ptr(), addr.len) == 0 ? 0 : fail();
}

int UdpSocket::connect(const SockAddr& addr) noexcept {
  return ::connect(fd_, addr.ptr(), addr.len) == 0 ? 0 : fail();
}

int UdpSocket::set_reuseaddr(bool on) noexcept {
  return set_int_opt(SOL_SOCKET, SO_REUSEADDR, on ? 1 : 0);
}

int UdpSocket::set_rcvbuf(int bytes) noexcept {
  if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUFFORCE, &bytes, sizeof(bytes)) == 0) return 0;
  return set_int_opt(SOL_SOCKET, SO_RCVBUF, bytes);
}

int UdpSocket::rcvbuf() const noexcept {
  int v = 0;
  socklen_t len = sizeof(v);
  if (::getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &v, &len) != 0) return -errno;
  return v;
}

int UdpSocket::set_sndbuf(int bytes) noexcept {
  return set_int_opt(SOL_SOCKET, SO_SNDBUF, bytes);
}

int UdpSocket::join_group(std::uint32_t group_be, const McastInterface& ifc) noexcept {
  ip_mreqn m{};
  m.imr_multiaddr.s_addr = group_be;
  m.imr_address.s_addr = ifc.addr_be;
  m.imr_ifindex = static_cast<int>(ifc.index);
  if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) != 0) return fail();
  return 0;
}

int UdpSocket::join_source_group(std::uint32_t group_be,
                                 std::uint32_t source_be,
                                 const McastInterface& ifc) noexcept {
  if (ifc.index != 0) {
    group_source_req r{};
    r.gsr_interface = ifc.index;
    auto* g = reinterpret_cast<sockaddr_in*>(&r.gsr_group);
    g->sin_family = AF_INET;
    g->sin_addr.s_addr = group_be;
    auto* s = reinterpret_cast<sockaddr_in*>(&r.gsr_source);
    s->sin_family = AF_INET;
    s->sin_addr.s_addr = source_be;
    if (::setsockopt(fd_, IPPROTO_IP, MCAST_JOIN_SOURCE_GROUP, &r, sizeof(r)) != 0) return fail();
    return 0;
  }
  ip_mreq_source m{};
  m.imr_multiaddr.s_addr = group_be;
  m.imr_interface.s_addr = ifc.addr_be;
  m.imr_sourceaddr.s_addr = source_be;
  if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, &m, sizeof(m)) != 0) return fail();
  return 0;
}

int UdpSocket::set_multicast_all(bool on) noexcept {
  return set_int_opt(IPPROTO_IP, IP_MULTICAST_ALL, on ? 1 : 0);
}

int UdpSocket::set_multicast_if(const McastInterface& ifc) noexcept {
  ip_mreqn m{};
  m.imr_address.s_addr = ifc.addr_be;
  m.imr_ifindex = static_cast<int>(ifc.index);
  if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &m, sizeof(m)) != 0) return fail();
  return 0;
}

int UdpSocket::set_multicast_ttl(int ttl) noexcept {
  return set_int_opt(IPPROTO_IP, IP_MULTICAST_TTL, ttl);
}

int UdpSocket::set_multicast_loop(bool on) noexcept {
  return set_int_opt(IPPROTO_IP, IP_MULTICAST_LOOP, on ? 1 : 0);
}

int UdpSocket::enable_rx_timestamps() noexcept {
  const int flags = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE |
                    SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE;
  return set_int_opt(SOL_SOCKET, SO_TIMESTAMPING, flags);
}

int UdpSocket::set_busy_poll(int usec) noexcept {
  return set_int_opt(SOL_SOCKET, SO_BUSY_POLL, usec);
}

int UdpSocket::set_prefer_busy_poll(bool on) noexcept {
  return set_int_opt(SOL_SOCKET, SO_PREFER_BUSY_POLL, on ? 1 : 0);
}

int UdpSocket::set_busy_poll_budget(int packets) noexcept {
  return set_int_opt(SOL_SOCKET, SO_BUSY_POLL_BUDGET, packets);
}

IoResult UdpSocket::recv_from(std::span<std::byte> buf, SockAddr* from) noexcept {
  for (;;) {
    SockAddr a;
    a.len = sizeof(a.storage);
    // MSG_TRUNC: the return value is the datagram's real length even when it did not fit.
    const ssize_t n = ::recvfrom(fd_, buf.data(), buf.size(), MSG_TRUNC, a.ptr(), &a.len);
    if (n >= 0) {
      if (from != nullptr) *from = a;
      return IoResult::done(static_cast<std::size_t>(n));
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN) return IoResult::wants_read();
    err_ = errno;
    return IoResult::syscall(errno);
  }
}

IoResult UdpSocket::send_to(std::span<const std::byte> buf, const SockAddr& to) noexcept {
  for (;;) {
    const ssize_t n = ::sendto(fd_, buf.data(), buf.size(), MSG_NOSIGNAL, to.ptr(), to.len);
    if (n >= 0) return IoResult::done(static_cast<std::size_t>(n));
    if (errno == EINTR) continue;
    if (errno == EAGAIN) return IoResult::wants_write();
    err_ = errno;
    return IoResult::syscall(errno);
  }
}

IoResult UdpSocket::send(std::span<const std::byte> buf) noexcept {
  for (;;) {
    const ssize_t n = ::send(fd_, buf.data(), buf.size(), MSG_NOSIGNAL);
    if (n >= 0) return IoResult::done(static_cast<std::size_t>(n));
    if (errno == EINTR) continue;
    if (errno == EAGAIN) return IoResult::wants_write();
    err_ = errno;
    return IoResult::syscall(errno);
  }
}

int UdpSocket::recv_batch(std::span<mmsghdr> msgs) noexcept {
  for (;;) {
    const int n =
        ::recvmmsg(fd_, msgs.data(), static_cast<unsigned>(msgs.size()), MSG_DONTWAIT, nullptr);
    if (n >= 0) return n;
    if (errno == EINTR) continue;
    if (errno == EAGAIN) return 0;
    return fail();
  }
}

int UdpSocket::send_batch(std::span<mmsghdr> msgs) noexcept {
  for (;;) {
    const int n = ::sendmmsg(
        fd_, msgs.data(), static_cast<unsigned>(msgs.size()), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n >= 0) return n;
    if (errno == EINTR) continue;
    if (errno == EAGAIN) return 0;
    return fail();
  }
}

std::optional<SockAddr> UdpSocket::local_addr() const noexcept {
  SockAddr a;
  a.len = sizeof(a.storage);
  if (::getsockname(fd_, a.ptr(), &a.len) != 0) return std::nullopt;
  return a;
}

void UdpSocket::close() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

}  // namespace fastmm::net
