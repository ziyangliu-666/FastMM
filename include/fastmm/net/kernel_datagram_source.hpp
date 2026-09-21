#pragma once
// `kernel` DatagramSource (ADR-0015, section 2): one UDP socket per subscription, bound to the
// group and port, SO_REUSEADDR, IP_MULTICAST_ALL off, joined any-source or source-specific;
// recvmmsg in fixed batches; SO_TIMESTAMPING for kernel and NIC receive times. Runs unmodified
// under Onload.
//
// open() allocates every buffer (headers, iovecs, addresses, control messages, payload slots:
// lines x batch x max_datagram bytes); poll() does not allocate. poll() makes one recvmmsg call
// per line and returns the datagrams delivered; with an edge-triggered reactor, call it until it
// returns 0 (every socket reported EAGAIN).
//
// NIC timestamps need SIOCSHWTSTAMP on the interface first (enable_hw_timestamps(), CAP_NET_ADMIN);
// SOF_TIMESTAMPING_RX_HARDWARE alone does nothing.
#include "fastmm/net/datagram_source.hpp"
#include "fastmm/net/udp_socket.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace fastmm::net {

struct DatagramSubscription {
  std::string interface;  // name ("eth1") or IPv4 address; "" = routing table
  std::string group;      // IPv4 multicast group
  std::uint16_t port = 0;
  std::string source;  // "" = any source; otherwise a source-specific join
};

struct KernelSourceConfig {
  std::vector<DatagramSubscription> subscriptions;  // line = index, at most 256
  int rcvbuf_bytes = 0;                             // 0 keeps the system default
  std::uint32_t batch = 32;                         // datagrams per recvmmsg, 1..1024
  std::uint32_t max_datagram = 2048;                // larger datagrams are dropped (truncated)
  bool timestamps = true;                           // SO_TIMESTAMPING
  // SO_BUSY_POLL / SO_PREFER_BUSY_POLL / SO_BUSY_POLL_BUDGET on every socket. Failures do not
  // fail open(); they are reported in KernelSourceOpen.
  bool busy_poll = false;
  int busy_poll_usec = 50;
  int busy_poll_budget = 0;  // 0 keeps the kernel default (8)
};

// Outcome of open(). `err` is 0 or -errno of the step that failed, for subscription `line`.
struct KernelSourceOpen {
  int err = 0;
  std::size_t line = 0;
  std::string_view step;  // "config", "interface", "group", "source", "socket", "bind", ...
  // Busy-poll options: 0 when applied to every socket, else the first -errno (-EPERM without
  // CAP_NET_ADMIN). Only set when busy_poll was requested.
  int busy_poll = 0;
  int prefer_busy_poll = 0;
  int busy_poll_budget = 0;
  int rcvbuf = 0;  // SO_RCVBUF granted on line 0, as getsockopt reports it

  [[nodiscard]] bool ok() const noexcept { return err == 0; }
  [[nodiscard]] bool busy_poll_applied() const noexcept {
    return busy_poll == 0 && prefer_busy_poll == 0 && busy_poll_budget == 0;
  }
};

class KernelDatagramSource {
 public:
  KernelDatagramSource() = default;
  ~KernelDatagramSource() = default;
  KernelDatagramSource(const KernelDatagramSource&) = delete;
  KernelDatagramSource& operator=(const KernelDatagramSource&) = delete;
  KernelDatagramSource(KernelDatagramSource&&) noexcept = default;
  KernelDatagramSource& operator=(KernelDatagramSource&&) noexcept = default;

  // Closes any previous sockets, then opens one per subscription. On failure nothing stays open.
  KernelSourceOpen open(const KernelSourceConfig& cfg);
  void close() noexcept;

  // handler(std::span<const std::byte> payload, const RxMeta& meta) noexcept, once per datagram.
  template <class H>
  // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward): called per datagram, never moved
  std::size_t poll(H&& handler) noexcept {
    static_assert(std::is_nothrow_invocable_v<H&, std::span<const std::byte>, const RxMeta&>,
                  "fastmm: DatagramSource handler must be void(std::span<const std::byte>, "
                  "const RxMeta&) noexcept");
    std::size_t delivered = 0;
    for (std::size_t line = 0; line < lines_.size(); ++line) {
      const std::size_t n = receive(line);
      const std::size_t base = line * batch_;
      for (std::size_t i = base; i < base + n; ++i) {
        if (lens_[i] == kDropped) continue;
        handler(std::span<const std::byte>(payload_.data() + i * max_datagram_, lens_[i]),
                metas_[i]);
        ++delivered;
      }
    }
    return delivered;
  }

  [[nodiscard]] const DatagramSourceStats& stats() const noexcept { return stats_; }
  [[nodiscard]] std::size_t line_count() const noexcept { return lines_.size(); }
  [[nodiscard]] int fd(std::size_t line) const noexcept {
    return line < lines_.size() ? lines_[line].sock.fd() : -1;
  }
  [[nodiscard]] bool is_open() const noexcept { return !lines_.empty(); }

 private:
  static constexpr std::uint32_t kDropped = 0xFFFF'FFFFU;

  struct Line {
    UdpSocket sock;
    std::uint32_t group_be = 0;
    std::uint16_t port = 0;
  };

  // recvmmsg on `line`: fills lens_/metas_ for the received slots, updates stats_, resets the
  // headers for the next call. Returns the number of slots used (dropped ones included).
  std::size_t receive(std::size_t line) noexcept;

  std::vector<Line> lines_;
  std::size_t batch_ = 0;
  std::size_t max_datagram_ = 0;
  std::size_t cmsg_space_ = 0;
  std::vector<mmsghdr> msgs_;  // lines x batch; each points at its slot's buffers below
  std::vector<iovec> iovs_;
  std::vector<sockaddr_in> names_;
  std::vector<std::byte> cmsgs_;
  std::vector<std::byte> payload_;
  std::vector<std::uint32_t> lens_;
  std::vector<RxMeta> metas_;
  DatagramSourceStats stats_;
};

static_assert(DatagramSource<KernelDatagramSource>);

}  // namespace fastmm::net
