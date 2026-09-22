#pragma once
// DPDK datagram source (rx_backend = "dpdk"): one ethdev port, one RX queue polled with
// rte_eth_rx_burst from the venue's network thread. Frames are parsed with parse_udp_frame
// (Ethernet, at most one 802.1Q tag, IPv4, UDP), matched against the subscriptions and handed to
// the handler; every mbuf of the burst is freed before poll() returns, so payload spans are valid
// only inside the handler call. A kernel UDP socket per subscription joins the group so the IGMP
// report goes out (which needs a kernel interface on that segment: the port's own netdev for
// af_packet/tap vdevs and bifurcated NICs, another one for a port bound to vfio-pci).
//
// Built only with -DFASTMM_WITH_DPDK=ON (FASTMM_HAS_DPDK=1); the header compiles without DPDK so
// the venue can name the type. No DPDK header is included here: poll() calls two out-of-line
// functions per burst.
//
// EAL: initialised once per process by the first open() with DpdkConfig::eal_args; a later open()
// must pass the same arguments. The calling thread's CPU affinity is restored after rte_eal_init.
// Unprivileged use (tests, bench over veth): "--no-huge --no-pci --in-memory --no-telemetry
// --vdev=net_af_packet0,iface=<netdev>" inside a user + network namespace (CAP_NET_RAW there).
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/net/datagram_source.hpp"
#include "fastmm/net/udp_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fastmm::net {

struct DpdkSubscription {
  std::string interface;     // kernel netdev for the IGMP join; "" = no join
  std::uint32_t group = 0;   // IPv4 multicast group, network byte order
  std::uint16_t port = 0;    // host byte order
  std::uint32_t source = 0;  // network byte order; 0 = any (joins only)
};

struct DpdkConfig {
  std::vector<std::string> eal_args;  // argv[1..] of rte_eal_init
  std::string port;                   // ethdev name ("net_af_packet0", "0000:00:06.0"); "" = first
  std::vector<DpdkSubscription> subscriptions;  // line = index, at most 256
  std::uint16_t rx_descriptors = 1024;
  std::uint32_t mbufs = 8191;
  std::uint32_t batch = 32;  // frames per rx burst, at most 256
  bool verify_udp_checksum = false;
};

struct DpdkStats {
  std::uint64_t datagrams = 0;
  std::uint64_t bytes = 0;
  std::uint64_t bad_frames = 0;  // truncated, bad IPv4 header, lengths or checksums, chained mbufs
  std::uint64_t other = 0;       // not IPv4 UDP (ARP, TCP, IGMP, fragments): not for this source
  std::uint64_t unmatched = 0;   // UDP, but no subscription
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

  template <class H>
  std::size_t poll(H&& handler) noexcept {
    const std::uint32_t n = rx_burst();
    if (n == 0) return 0;
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
    return delivered;
  }

  // rte_eth_stats_get (not for the hot loop).
  int refresh_stats() noexcept;
  [[nodiscard]] const DpdkStats& stats() const noexcept { return stats_; }

 private:
  struct Route {
    std::uint32_t dst_ip;
    std::uint16_t dst_port;
    std::uint8_t line;
  };

  std::uint32_t rx_burst() noexcept;  // fills frames_ (empty span: chained mbuf)
  void free_burst(std::uint32_t n) noexcept;
  int fail(int err, std::string msg);

  std::vector<std::span<const std::byte>> frames_;
  std::vector<void*> mbufs_;  // rte_mbuf*
  std::vector<Route> routes_;
  std::vector<int> join_fds_;
  void* pool_ = nullptr;  // rte_mempool*
  std::string error_;
  std::string port_name_;
  DpdkStats stats_{};
  std::uint16_t port_ = 0;
  std::uint16_t batch_ = 32;
  bool started_ = false;
  bool verify_checksums_ = false;
};

static_assert(DatagramSource<DpdkDatagramSource>);

}  // namespace fastmm::net
