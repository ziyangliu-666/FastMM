#pragma once
// AF_XDP datagram source (ADR-0015, section 3): one XDP socket per (interface, RX queue), each with
// its own UMEM, fill and RX rings (the completion ring exists because bind requires it). A BPF
// program per interface, loaded and attached over raw bpf(2) calls, redirects UDP datagrams for
// the subscribed (group, port) pairs to the sockets and passes everything else to the kernel. A
// kernel UDP socket per multicast subscription joins the group so IGMP reports go out; it reads
// nothing. A unicast subscription is a local address (no join). Linux 5.11 or later; needs
// CAP_NET_ADMIN, CAP_NET_RAW, CAP_BPF and CAP_IPC_LOCK.
//
// Single-threaded: open, poll and the stats calls belong to the net thread. poll() allocates
// nothing and returns every RX descriptor it took to the fill ring before it returns, so payload
// spans are valid only inside the handler call.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/net/datagram_source.hpp"
#include "fastmm/net/udp_frame.hpp"
#include "fastmm/net/xdp_program.hpp"

#include <sys/socket.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fastmm::net {

// Attach and bind mode. Auto tries ZeroCopy, then NativeCopy, then Generic.
//   ZeroCopy    native XDP (driver), socket bound with XDP_ZEROCOPY
//   NativeCopy  native XDP, socket bound with XDP_COPY
//   Generic     generic XDP (XDP_FLAGS_SKB_MODE), socket bound with XDP_COPY
enum class XdpMode : std::uint8_t { Auto, ZeroCopy, NativeCopy, Generic };

[[nodiscard]] constexpr std::string_view to_string(XdpMode m) noexcept {
  switch (m) {
    case XdpMode::Auto:
      return "auto";
    case XdpMode::ZeroCopy:
      return "zerocopy";
    case XdpMode::NativeCopy:
      return "native_copy";
    case XdpMode::Generic:
      return "generic";
  }
  return "?";
}

struct XdpSubscription {
  std::string interface;     // netdev name
  std::uint32_t group = 0;   // IPv4 multicast group or local unicast address, network byte order
  std::uint16_t port = 0;    // host byte order
  std::uint32_t source = 0;  // network byte order; 0 = any. Joins only: the XDP map ignores it.
};

struct XdpInterfaceQueues {
  std::string interface;
  std::vector<std::uint32_t> queues;
};

struct XdpConfig {
  std::vector<XdpSubscription> subscriptions;  // line = index, at most 256
  // RX queues per interface. Unlisted: every RX queue the interface has once the program is
  // attached (virtio_net, for one, adds queues for XDP and the host delivers on all of them).
  std::vector<XdpInterfaceQueues> queues;
  std::uint32_t frame_count = 4096;  // UMEM frames per socket, power of two
  std::uint32_t frame_size = 4096;   // 2048 or 4096 (at most the page size)
  std::uint32_t batch = 64;          // RX descriptors per socket per poll
  XdpMode mode = XdpMode::Auto;
  // SO_PREFER_BUSY_POLL, SO_BUSY_POLL and SO_BUSY_POLL_BUDGET on every socket, and a recvfrom
  // on every poll that finds the RX ring empty (drives NAPI from this thread).
  bool busy_poll = false;
  int busy_poll_usec = 20;
  int busy_poll_budget = 64;
  // IPv4 header and UDP checksums in user space (a zero UDP checksum is accepted).
  bool verify_udp_checksum = false;
};

struct XdpStats {
  std::uint64_t datagrams = 0;   // delivered to the handler
  std::uint64_t bytes = 0;       // payload bytes delivered
  std::uint64_t bad_frames = 0;  // failed the user-space checks (udp_frame.hpp)
  std::uint64_t unmatched = 0;   // valid, but no subscription on that interface
  std::uint64_t fill_short = 0;  // descriptors not returned because the fill ring was full
};

// XDP_STATISTICS of one socket, as of the last refresh_stats().
struct XdpSocketStats {
  unsigned ifindex = 0;
  std::uint32_t queue = 0;
  std::uint64_t rx_dropped = 0;
  std::uint64_t rx_invalid_descs = 0;
  std::uint64_t rx_ring_full = 0;
  std::uint64_t rx_fill_ring_empty_descs = 0;
};

struct XdpInterfaceStatus {
  std::string name;
  unsigned ifindex = 0;
  XdpMode mode = XdpMode::Auto;        // the mode open() settled on
  std::uint64_t fallback_packets = 0;  // subscribed, but no socket on the RX queue: XDP_PASS
};

// "" when the process has CAP_NET_ADMIN, CAP_NET_RAW, CAP_BPF (or CAP_SYS_ADMIN) and CAP_IPC_LOCK
// in its effective set; otherwise names the missing ones and the setcap command.
[[nodiscard]] std::string xdp_capability_error();
// "" on Linux 5.11 or later; otherwise the reason.
[[nodiscard]] std::string xdp_kernel_error();

// The BPF objects of one interface: subscription hash map, XSKMAP, per-CPU fallback counter and
// the program, plus its XDP link once attached. Closing the link fd detaches the program.
class XdpFilter {
 public:
  XdpFilter() = default;
  ~XdpFilter() { close(); }
  XdpFilter(const XdpFilter&) = delete;
  XdpFilter& operator=(const XdpFilter&) = delete;
  XdpFilter(XdpFilter&& o) noexcept { swap(o); }
  XdpFilter& operator=(XdpFilter&& o) noexcept {
    if (this != &o) {
      close();
      swap(o);
    }
    return *this;
  }

  // Creates the maps (XSKMAP with `xsk_slots` entries), inserts `keys` and loads the program.
  // Returns 0 or -errno; `err` names the failing step and carries the verifier log when the load
  // fails.
  int create(std::span<const xdp::Key> keys, std::uint32_t xsk_slots, std::string& err);
  // BPF_LINK_CREATE (attach type BPF_XDP) on `ifindex` with XDP_FLAGS_DRV_MODE or
  // XDP_FLAGS_SKB_MODE. Returns 0 or -errno: -EBUSY or -EEXIST when the interface already has an
  // XDP program, -EOPNOTSUPP when the driver has no native XDP.
  int attach(unsigned ifindex, bool generic) noexcept;
  void detach() noexcept;
  // xsks[queue] = xsk_fd. The socket must be bound.
  int set_socket(std::uint32_t queue, int xsk_fd) noexcept;
  // Sum of the per-CPU fallback counters.
  int fallback_count(std::uint64_t& out) noexcept;
  // BPF_PROG_TEST_RUN on `frame` (at least 14 bytes). Returns 0 or -errno; the XDP action in
  // `action`.
  int test_run(std::span<const std::byte> frame, std::uint32_t& action) noexcept;

  [[nodiscard]] int prog_fd() const noexcept { return prog_fd_; }
  [[nodiscard]] int link_fd() const noexcept { return link_fd_; }
  void close() noexcept;

 private:
  void swap(XdpFilter& o) noexcept {
    std::swap(subs_fd_, o.subs_fd_);
    std::swap(xsks_fd_, o.xsks_fd_);
    std::swap(fallback_fd_, o.fallback_fd_);
    std::swap(prog_fd_, o.prog_fd_);
    std::swap(link_fd_, o.link_fd_);
    percpu_.swap(o.percpu_);
  }

  int subs_fd_ = -1;
  int xsks_fd_ = -1;
  int fallback_fd_ = -1;
  int prog_fd_ = -1;
  int link_fd_ = -1;
  std::vector<std::uint64_t> percpu_;  // one slot per possible CPU
};

namespace detail {

// A single-producer or single-consumer AF_XDP ring shared with the kernel (libxdp's xsk_ring_*).
struct XskRing {
  std::atomic<std::uint32_t>* producer = nullptr;
  std::atomic<std::uint32_t>* consumer = nullptr;
  std::atomic<std::uint32_t>* flags = nullptr;
  void* entries = nullptr;
  std::uint32_t mask = 0;
  std::uint32_t size = 0;
  std::uint32_t cached_prod = 0;
  std::uint32_t cached_cons = 0;  // producer rings: consumer + size

  // Consumer side: up to `max` entries starting at `idx`.
  std::uint32_t peek(std::uint32_t max, std::uint32_t& idx) noexcept {
    std::uint32_t avail = cached_prod - cached_cons;
    if (avail < max) {
      cached_prod = producer->load(std::memory_order_acquire);
      avail = cached_prod - cached_cons;
    }
    const std::uint32_t n = avail < max ? avail : max;
    idx = cached_cons;
    cached_cons += n;
    return n;
  }
  void release() noexcept { consumer->store(cached_cons, std::memory_order_release); }

  // Producer side: `n` slots starting at `idx`, or 0 when fewer are free.
  std::uint32_t reserve(std::uint32_t n, std::uint32_t& idx) noexcept {
    if (cached_cons - cached_prod < n) {
      cached_cons = consumer->load(std::memory_order_acquire) + size;
      if (cached_cons - cached_prod < n) return 0;
    }
    idx = cached_prod;
    cached_prod += n;
    return n;
  }
  void submit() noexcept { producer->store(cached_prod, std::memory_order_release); }
  [[nodiscard]] bool needs_wakeup() const noexcept {
    return (flags->load(std::memory_order_relaxed) & 1U) != 0;  // XDP_RING_NEED_WAKEUP
  }
};

struct XdpDescView {
  std::uint64_t addr;
  std::uint32_t len;
  std::uint32_t options;
};

struct XdpRoute {
  std::uint32_t dst_ip;    // network byte order
  std::uint16_t dst_port;  // host byte order
  std::uint8_t line;
};

struct XskSocket {
  int fd = -1;
  unsigned ifindex = 0;
  std::uint32_t queue = 0;
  std::uint32_t routes_begin = 0;  // this interface's routes in XdpDatagramSource::routes_
  std::uint32_t routes_end = 0;
  std::byte* umem = nullptr;
  std::size_t umem_len = 0;
  void* fill_map = nullptr;
  std::size_t fill_map_len = 0;
  void* rx_map = nullptr;
  std::size_t rx_map_len = 0;
  XskRing fill;
  XskRing rx;
};

}  // namespace detail

class XdpDatagramSource {
 public:
  XdpDatagramSource() = default;
  ~XdpDatagramSource() { close(); }
  XdpDatagramSource(const XdpDatagramSource&) = delete;
  XdpDatagramSource& operator=(const XdpDatagramSource&) = delete;
  XdpDatagramSource(XdpDatagramSource&&) = delete;
  XdpDatagramSource& operator=(XdpDatagramSource&&) = delete;

  // Returns 0 or -errno; error() explains. Fails with -EPERM and the setcap command when
  // capabilities are missing, -ENOSYS before Linux 5.11, -EBUSY when an interface already has an
  // XDP program. Never falls back to the kernel backend.
  int open(const XdpConfig& cfg);
  void close() noexcept;
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  // Busy-poll options the kernel refused at open (the sockets poll from user space without them).
  [[nodiscard]] std::span<const std::string> warnings() const noexcept { return warnings_; }

  // Delivers up to `batch` datagrams per socket; returns the number delivered.
  template <class H>
  std::size_t poll(H&& handler) noexcept {
    std::size_t total = 0;
    for (auto& s : sockets_) total += poll_socket(s, handler);
    return total;
  }

  // XDP socket fds, readable when their RX ring has descriptors (adaptive mode waits on them).
  [[nodiscard]] std::span<const int> fds() const noexcept { return fds_; }

  // Reads XDP_STATISTICS and the fallback counters (system calls; not for the hot loop).
  int refresh_stats() noexcept;
  [[nodiscard]] const XdpStats& stats() const noexcept { return stats_; }
  [[nodiscard]] std::span<const XdpSocketStats> socket_stats() const noexcept {
    return socket_stats_;
  }
  [[nodiscard]] std::span<const XdpInterfaceStatus> interfaces() const noexcept {
    return interfaces_;
  }
  // The filter of interfaces()[i] (tests run BPF_PROG_TEST_RUN on the attached program).
  [[nodiscard]] XdpFilter& filter(std::size_t i) noexcept { return filters_[i]; }

 private:
  template <class H>
  std::size_t poll_socket(detail::XskSocket& s, H& handler) noexcept {
    std::uint32_t rx_idx = 0;
    const std::uint32_t n = s.rx.peek(batch_, rx_idx);
    if (n == 0) {
      if (busy_poll_ || s.fill.needs_wakeup()) wake(s);
      return 0;
    }
    RxMeta meta{};
    meta.t0_cycles = rdtscp();
    meta.t0_wall_ns = wall_now().ns;
    std::uint32_t fill_idx = 0;
    const bool can_fill = s.fill.reserve(n, fill_idx) == n;
    const auto* descs = static_cast<const detail::XdpDescView*>(s.rx.entries);
    auto* fill_addrs = static_cast<std::uint64_t*>(s.fill.entries);
    std::size_t delivered = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
      const detail::XdpDescView d = descs[(rx_idx + i) & s.rx.mask];
      if (can_fill) fill_addrs[(fill_idx + i) & s.fill.mask] = d.addr & frame_mask_;
      if (FASTMM_UNLIKELY(d.addr > s.umem_len || d.len > s.umem_len - d.addr)) {
        ++stats_.bad_frames;
        continue;
      }
      const std::span<const std::byte> frame(s.umem + d.addr, d.len);
      const UdpFrame f = parse_udp_frame(frame, verify_checksums_);
      if (FASTMM_UNLIKELY(f.status != FrameStatus::Ok)) {
        ++stats_.bad_frames;
        continue;
      }
      const detail::XdpRoute* route = nullptr;
      for (std::uint32_t r = s.routes_begin; r < s.routes_end; ++r) {
        if (routes_[r].dst_ip == f.dst_ip && routes_[r].dst_port == f.dst_port) {
          route = &routes_[r];
          break;
        }
      }
      if (FASTMM_UNLIKELY(route == nullptr)) {
        ++stats_.unmatched;
        continue;
      }
      meta.src_ip = f.src_ip;
      meta.dst_ip = f.dst_ip;
      meta.dst_port = f.dst_port;
      meta.line = route->line;
      handler(f.payload, static_cast<const RxMeta&>(meta));
      ++delivered;
      stats_.bytes += f.payload.size();
    }
    s.rx.release();
    if (can_fill) {
      s.fill.submit();
    } else {
      stats_.fill_short += n;  // cannot happen: the fill ring holds every frame of the UMEM
    }
    stats_.datagrams += delivered;
    return delivered;
  }

  static void wake(const detail::XskSocket& s) noexcept {
    ::recvfrom(s.fd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
  }

  int fail(int err, std::string msg);
  int open_interface(std::size_t iface,
                     std::span<const std::uint32_t> listed,
                     std::uint32_t routes_begin,
                     std::uint32_t routes_end,
                     const XdpConfig& cfg);
  int create_socket(detail::XskSocket& s, bool zerocopy, const XdpConfig& cfg, std::string& why);
  static void close_socket(detail::XskSocket& s) noexcept;

  std::vector<detail::XskSocket> sockets_;
  std::vector<detail::XdpRoute> routes_;
  std::vector<int> fds_;
  std::vector<XdpFilter> filters_;
  std::vector<int> join_fds_;
  std::vector<XdpInterfaceStatus> interfaces_;
  std::vector<XdpSocketStats> socket_stats_;
  std::vector<std::string> warnings_;
  std::string error_;
  XdpStats stats_;
  std::uint64_t frame_mask_ = 0;
  std::uint32_t batch_ = 64;
  bool busy_poll_ = false;
  bool verify_checksums_ = false;
};

static_assert(DatagramSource<XdpDatagramSource>);

}  // namespace fastmm::net
