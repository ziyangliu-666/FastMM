#pragma once
// DPDK datagram source (rx_backend = "dpdk"): one ethdev port, one RX and one TX queue polled from
// the venue's network thread. Frames are parsed with parse_udp_frame (Ethernet, at most one 802.1Q
// tag, IPv4, UDP), matched against the subscriptions and handed to the handler; every mbuf of the
// burst is freed (or passed on) before poll() returns, so payload spans are valid only inside the
// handler call.
//
// Frames that are not a subscribed datagram:
//   - ARP, and TCP to the FrameSink's address (and port), go to the sink: UserTcp on the same port
//     (order_transport = "user_tcp"), sending through frame_tx()
//   - with an exception port (a net_tap vdev), everything else goes to the kernel through it, and
//     what the kernel sends there goes out of the port: the kernel keeps an interface with the
//     port's MAC and exception_ip (ARP, ICMP, IGMP joins, GLIMPSE, re-requests, kernel TCP). The
//     tap is read every exception_interval_ns (a system call per read).
//   - without one, ARP requests for a unicast subscription address are answered here; the rest
//     is dropped
//
// Multicast: a kernel UDP socket per subscription joins the group on `interface` so the IGMP
// report goes out (the exception interface when the port is bound to vfio-pci; the port's own
// netdev for af_packet vdevs). A unicast subscription (a local address) needs no join.
//
// Built only with -DFASTMM_WITH_DPDK=ON (FASTMM_HAS_DPDK=1); the header compiles without DPDK so
// the venue can name the type. No DPDK header is included here.
//
// EAL: initialised once per process by the first open() with DpdkConfig::eal_args; a later open()
// must pass the same arguments. The calling thread's CPU affinity is restored after rte_eal_init.
// Unprivileged use (tests, bench over veth): "--no-huge --no-pci --in-memory --no-telemetry
// --vdev=net_af_packet0,iface=<netdev>" inside a user + network namespace (CAP_NET_RAW there).
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/net/datagram_source.hpp"
#include "fastmm/net/udp_frame.hpp"
#include "fastmm/net/user_tcp.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fastmm::net {

struct DpdkSubscription {
  std::string interface;     // kernel netdev for the IGMP join; "" = no join
  std::uint32_t group = 0;   // IPv4 multicast group or local unicast address, network byte order
  std::uint16_t port = 0;    // host byte order
  std::uint32_t source = 0;  // network byte order; 0 = any (joins only)
};

struct DpdkConfig {
  std::vector<std::string> eal_args;  // argv[1..] of rte_eal_init
  std::string port;                   // ethdev name ("net_af_packet0", "0000:00:06.0"); "" = first
  std::vector<DpdkSubscription> subscriptions;  // line = index, at most 256
  std::uint16_t rx_descriptors = 1024;
  std::uint16_t tx_descriptors = 512;
  std::uint32_t mbufs = 8191;
  std::uint32_t batch = 32;  // frames per rx burst, at most 256
  bool verify_udp_checksum = false;
  // Kernel exception path: ethdev name of a net_tap vdev ("" = none). Its netdev gets the port's
  // MAC and exception_ip ("a.b.c.d/len", "" = leave it unconfigured) and is brought up.
  std::string exception_port;
  std::string exception_ip;
  std::int64_t exception_interval_ns = 20'000;  // 0 = read the tap on every poll
};

struct DpdkStats {
  std::uint64_t datagrams = 0;
  std::uint64_t bytes = 0;
  std::uint64_t bad_frames = 0;   // truncated, bad IPv4 header, lengths or checksums, chained mbufs
  std::uint64_t other = 0;        // not IPv4 UDP (ARP, TCP, IGMP, fragments): not for this source
  std::uint64_t unmatched = 0;    // UDP, but no subscription
  std::uint64_t to_sink = 0;      // frames given to the FrameSink
  std::uint64_t to_kernel = 0;    // frames passed to the exception port
  std::uint64_t from_kernel = 0;  // frames the kernel sent through the exception port
  std::uint64_t arp_replies = 0;  // answered here (no exception port)
  std::uint64_t tx_frames = 0;    // frame_tx() frames handed to the port
  std::uint64_t tx_drops = 0;     // no mbuf, or the TX queue was full
  // rte_eth_stats_get, as of the last refresh_stats()
  std::uint64_t ipackets = 0;
  std::uint64_t imissed = 0;
  std::uint64_t ierrors = 0;
  std::uint64_t rx_nombuf = 0;
};

// True when this build has the DPDK backend.
[[nodiscard]] bool dpdk_available() noexcept;

class DpdkDatagramSource {
 public:
  DpdkDatagramSource() = default;
  ~DpdkDatagramSource() { close(); }
  DpdkDatagramSource(const DpdkDatagramSource&) = delete;
  DpdkDatagramSource& operator=(const DpdkDatagramSource&) = delete;
  DpdkDatagramSource(DpdkDatagramSource&&) = delete;
  DpdkDatagramSource& operator=(DpdkDatagramSource&&) = delete;

  // Returns 0 or -errno; error() explains. -ENOTSUP without DPDK in the build.
  int open(const DpdkConfig& cfg);
  void close() noexcept;
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  [[nodiscard]] const std::string& port_name() const noexcept { return port_name_; }
  // The exception port's netdev ("" without one).
  [[nodiscard]] const std::string& exception_interface() const noexcept { return exc_ifname_; }

  // Frames for a user-space protocol on this port (see FrameSink). Set after open().
  void set_frame_sink(const FrameSink& sink) noexcept { sink_ = sink; }
  // Sends Ethernet frames out of the port (one copy into an mbuf; flush() transmits).
  [[nodiscard]] FrameTx& frame_tx() noexcept { return tx_; }
  [[nodiscard]] const MacAddr& mac() const noexcept { return mac_; }
  [[nodiscard]] std::uint32_t mtu() const noexcept { return mtu_; }

  template <class H>
  std::size_t poll(H&& handler) noexcept {
    const std::uint32_t n = rx_burst();
    if (n == 0) {
      if (FASTMM_UNLIKELY(exc_ != kNoPort)) service_exception();
      return 0;
    }
    RxMeta meta{};
    meta.t0_cycles = rdtscp();
    meta.t0_wall_ns = wall_now().ns;
    std::size_t delivered = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
      const UdpFrame f = parse_udp_frame(frames_[i], verify_checksums_);
      if (FASTMM_UNLIKELY(f.status != FrameStatus::Ok)) {
        const bool other = f.status == FrameStatus::NotIpv4 || f.status == FrameStatus::NotUdp ||
                           f.status == FrameStatus::Fragment;
        ++(other ? stats_.other : stats_.bad_frames);
        if (other) divert(i);
        continue;
      }
      const Route* route = nullptr;
      for (const Route& r : routes_) {
        if (r.dst_ip == f.dst_ip && r.dst_port == f.dst_port) {
          route = &r;
          break;
        }
      }
      if (route == nullptr) {
        ++stats_.unmatched;
        divert(i);
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
    free_burst(n);
    stats_.datagrams += delivered;
    if (FASTMM_UNLIKELY(exc_ != kNoPort)) service_exception();
    return delivered;
  }

  // rte_eth_stats_get (not for the hot loop).
  int refresh_stats() noexcept;
  [[nodiscard]] const DpdkStats& stats() const noexcept { return stats_; }

 private:
  static constexpr std::uint16_t kNoPort = 0xFFFF;

  struct Route {
    std::uint32_t dst_ip;
    std::uint16_t dst_port;
    std::uint8_t line;
  };
  class Tx final : public FrameTx {
   public:
    explicit Tx(DpdkDatagramSource& s) noexcept : s_(s) {}
    bool send_frame(std::span<const std::byte> frame) noexcept override {
      return s_.tx_frame(frame);
    }
    void flush() noexcept override { s_.tx_flush(); }

   private:
    DpdkDatagramSource& s_;
  };

  std::uint32_t rx_burst() noexcept;  // fills frames_ (empty span: chained mbuf)
  void free_burst(std::uint32_t n) noexcept;
  // Frame i was not delivered: the sink, the exception port or an ARP answer (out of line).
  void divert(std::uint32_t i) noexcept;
  // Passes the diverted frames to the kernel and, when due, the kernel's frames to the port.
  void service_exception() noexcept;
  bool tx_frame(std::span<const std::byte> frame) noexcept;
  void tx_flush() noexcept;
  int open_exception(const DpdkConfig& cfg, unsigned sock);
  int fail(int err, std::string msg);

  std::vector<std::span<const std::byte>> frames_;
  std::vector<void*> mbufs_;    // rte_mbuf*
  std::vector<void*> to_exc_;   // mbufs for the exception port
  std::vector<void*> tx_pend_;  // frame_tx() mbufs not yet transmitted
  std::vector<void*> exc_rx_;   // burst read from the exception port
  std::vector<Route> routes_;
  std::vector<std::uint32_t> unicast_;  // unicast subscription addresses (ARP answers)
  std::vector<int> join_fds_;
  void* pool_ = nullptr;  // rte_mempool*
  std::string error_;
  std::string port_name_;
  std::string exc_ifname_;
  DpdkStats stats_{};
  FrameSink sink_{};
  Tx tx_{*this};
  MacAddr mac_{};
  std::uint32_t mtu_ = 1500;
  std::uint32_t to_exc_n_ = 0;
  std::uint32_t tx_pend_n_ = 0;
  std::uint64_t exc_interval_cycles_ = 0;
  std::uint64_t exc_next_ = 0;
  std::uint16_t port_ = 0;
  std::uint16_t exc_ = kNoPort;
  std::uint16_t batch_ = 32;
  bool started_ = false;
  bool exc_started_ = false;
  bool verify_checksums_ = false;
};

static_assert(DatagramSource<DpdkDatagramSource>);

}  // namespace fastmm::net
