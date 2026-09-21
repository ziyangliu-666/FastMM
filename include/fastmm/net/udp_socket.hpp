#pragma once
// Non-blocking IPv4 UDP socket with RAII fd ownership: multicast membership, batch receive
// (recvmmsg) and send (sendmmsg), SO_TIMESTAMPING and busy-poll options. Linux only.
//
// Configuration calls return 0 or -errno and also keep the errno in last_error(). I/O returns
// IoResult (single datagram) or a count / -errno (batches). Nothing here allocates or throws.
#include "fastmm/net/io_result.hpp"
#include "fastmm/net/tcp_socket.hpp"

#include <sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace fastmm::net {

// Parses dotted-quad IPv4 into network byte order. Returns false on anything else.
[[nodiscard]] bool parse_ipv4(std::string_view text, std::uint32_t& out_be) noexcept;
[[nodiscard]] constexpr bool is_ipv4_multicast(std::uint32_t addr_be) noexcept {
  return (addr_be & 0xF0U) == 0xE0U;  // first octet 224..239, lowest byte in network order
}

// Interface for multicast membership and IP_MULTICAST_IF: an IPv4 address or an interface index.
// Both zero lets the kernel pick the interface from the routing table.
struct McastInterface {
  std::uint32_t addr_be = 0;
  unsigned index = 0;

  // "" (kernel's choice), a dotted-quad address, or an interface name ("eth0", resolved with
  // if_nametoindex). Returns 0 or -errno (-ENODEV for an unknown name).
  [[nodiscard]] static int resolve(std::string_view spec, McastInterface& out) noexcept;
};

// SIOCSHWTSTAMP on `ifname`: hardware timestamps for all received packets, TX off. Needs
// CAP_NET_ADMIN and a NIC that supports it; the setting is device-wide and outlives the process.
// Returns 0 or -errno (-EOPNOTSUPP / -EINVAL without NIC support, -EPERM without the capability).
[[nodiscard]] int enable_hw_timestamps(std::string_view ifname) noexcept;

class UdpSocket {
 public:
  UdpSocket() = default;
  explicit UdpSocket(int fd) noexcept : fd_(fd) {}
  ~UdpSocket() { close(); }
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;
  UdpSocket(UdpSocket&& o) noexcept : fd_(o.fd_), err_(o.err_) { o.fd_ = -1; }
  UdpSocket& operator=(UdpSocket&& o) noexcept {
    if (this != &o) {
      close();
      fd_ = o.fd_;
      err_ = o.err_;
      o.fd_ = -1;
    }
    return *this;
  }

  // Non-blocking, close-on-exec AF_INET datagram socket. Invalid on failure (errno in
  // last_error()).
  [[nodiscard]] static UdpSocket open() noexcept;

  [[nodiscard]] int bind(const SockAddr& addr) noexcept;
  [[nodiscard]] int connect(const SockAddr& addr) noexcept;
  [[nodiscard]] int set_reuseaddr(bool on) noexcept;
  // SO_RCVBUFFORCE (CAP_NET_ADMIN), falling back to SO_RCVBUF, which net.core.rmem_max caps.
  // rcvbuf() reports what the kernel granted (twice the request, for its bookkeeping).
  [[nodiscard]] int set_rcvbuf(int bytes) noexcept;
  [[nodiscard]] int rcvbuf() const noexcept;
  [[nodiscard]] int set_sndbuf(int bytes) noexcept;

  // IP_ADD_MEMBERSHIP (any source) and source-specific membership (IP_ADD_SOURCE_MEMBERSHIP for
  // an interface address, MCAST_JOIN_SOURCE_GROUP for an index). Addresses in network order.
  [[nodiscard]] int join_group(std::uint32_t group_be, const McastInterface& ifc) noexcept;
  [[nodiscard]] int join_source_group(std::uint32_t group_be,
                                      std::uint32_t source_be,
                                      const McastInterface& ifc) noexcept;
  // IP_MULTICAST_ALL: off delivers only the groups this socket joined. Linux defaults to on,
  // which also delivers groups joined by other sockets that match the bound address.
  [[nodiscard]] int set_multicast_all(bool on) noexcept;
  [[nodiscard]] int set_multicast_if(const McastInterface& ifc) noexcept;
  [[nodiscard]] int set_multicast_ttl(int ttl) noexcept;
  [[nodiscard]] int set_multicast_loop(bool on) noexcept;

  // SO_TIMESTAMPING with RX_SOFTWARE | SOFTWARE | RX_HARDWARE | RAW_HARDWARE. Both stamps arrive in
  // an SCM_TIMESTAMPING control message: ts[0] is the kernel's CLOCK_REALTIME receive time,
  // ts[2] the NIC's raw PHC time, which is present only after enable_hw_timestamps() on the
  // interface and is not CLOCK_REALTIME unless the PHC is disciplined to it (phc2sys).
  [[nodiscard]] int enable_rx_timestamps() noexcept;
  // SO_BUSY_POLL (µs), SO_PREFER_BUSY_POLL and SO_BUSY_POLL_BUDGET (both Linux 5.11). Each
  // returns the setsockopt errno and changes nothing on failure: -EPERM without CAP_NET_ADMIN in
  // the initial namespace (SO_BUSY_POLL above the current value before Linux 6.4, enabling
  // SO_PREFER_BUSY_POLL, raising SO_BUSY_POLL_BUDGET), -ENOPROTOOPT on older kernels.
  [[nodiscard]] int set_busy_poll(int usec) noexcept;
  [[nodiscard]] int set_prefer_busy_poll(bool on) noexcept;
  [[nodiscard]] int set_busy_poll_budget(int packets) noexcept;

  // One datagram. would_block() on EAGAIN; `bytes` is the datagram's full length, which can
  // exceed buf.size() (the rest is discarded; MSG_TRUNC).
  IoResult recv_from(std::span<std::byte> buf, SockAddr* from = nullptr) noexcept;
  IoResult send_to(std::span<const std::byte> buf, const SockAddr& to) noexcept;
  IoResult send(std::span<const std::byte> buf) noexcept;
  // recvmmsg / sendmmsg without blocking: messages transferred, 0 when the socket would block,
  // or -errno. The caller owns the headers and resets msg_namelen / msg_controllen between calls.
  int recv_batch(std::span<mmsghdr> msgs) noexcept;
  int send_batch(std::span<mmsghdr> msgs) noexcept;

  std::optional<SockAddr> local_addr() const noexcept;

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
  int set_int_opt(int level, int name, int value) noexcept;
  int fail() noexcept;

  int fd_ = -1;
  int err_ = 0;
};

}  // namespace fastmm::net
