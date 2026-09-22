#pragma once
// PacketRing: an AF_PACKET socket with TPACKET_V2 RX and TX rings mapped into user space, bound
// to one interface: the frame device under UserTcp (user_tcp.hpp). Receiving reads the RX ring
// with no system call; sending writes TX ring slots and flush() makes one sendto() for all of
// them (PACKET_QDISC_BYPASS: straight to the driver). A classic BPF filter keeps only ARP and
// IPv4 TCP to `local_ip`; PACKET_IGNORE_OUTGOING hides the socket's own frames.
//
// Needs CAP_NET_RAW in the interface's network namespace (a user namespace is enough). AF_PACKET
// sees a copy of each frame: the kernel stack still gets them, which is why UserTcp's address
// must not be assigned to a kernel interface.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/net/user_tcp.hpp"

#include <linux/if_packet.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace fastmm::net {

struct PacketRingConfig {
  std::string interface;
  std::uint32_t local_ip = 0;  // network byte order: the filter's IPv4 destination; 0 = any TCP
  std::uint32_t rx_frames = 512;
  std::uint32_t tx_frames = 128;
  bool qdisc_bypass = true;
};

struct PacketRingStats {
  std::uint64_t rx_frames = 0;
  std::uint64_t tx_frames = 0;
  std::uint64_t tx_ring_full = 0;  // send_frame() found the next slot still in use
  std::uint64_t tx_errors = 0;     // failed sendto() kicks
  int last_error = 0;
};

class PacketRing final : public FrameTx {
 public:
  static constexpr std::uint32_t kFrameSize = 2048;

  PacketRing() = default;
  ~PacketRing() override { close(); }
  PacketRing(const PacketRing&) = delete;
  PacketRing& operator=(const PacketRing&) = delete;
  PacketRing(PacketRing&&) = delete;
  PacketRing& operator=(PacketRing&&) = delete;

  // Returns 0 or -errno; `err` names the failing step.
  int open(const PacketRingConfig& cfg, std::string& err);
  void close() noexcept;

  // Hands each received frame to f(frame, csum_unverified) and returns the slot to the kernel;
  // returns the number of frames. csum_unverified: the sender left the L4 checksum to offload.
  template <class F>
  std::size_t poll(F&& f, std::size_t max = 64) noexcept {
    std::size_t n = 0;
    while (n < max) {
      auto* hdr =
          reinterpret_cast<tpacket2_hdr*>(rx_ + static_cast<std::size_t>(rx_idx_) * kFrameSize);
      const std::uint32_t status =
          std::atomic_ref<std::uint32_t>(hdr->tp_status).load(std::memory_order_acquire);
      if ((status & TP_STATUS_USER) == 0) break;
      const auto* data = reinterpret_cast<const std::byte*>(hdr) + hdr->tp_mac;
      f(std::span<const std::byte>(data, hdr->tp_snaplen),
        (status & (TP_STATUS_CSUMNOTREADY | TP_STATUS_CSUM_VALID)) != 0);
      std::atomic_ref<std::uint32_t>(hdr->tp_status)
          .store(TP_STATUS_KERNEL, std::memory_order_release);
      rx_idx_ = rx_idx_ + 1 == rx_count_ ? 0 : rx_idx_ + 1;
      ++n;
    }
    stats_.rx_frames += n;
    return n;
  }

  bool send_frame(std::span<const std::byte> frame) noexcept override;
  void flush() noexcept override;

  [[nodiscard]] int fd() const noexcept { return fd_; }
  [[nodiscard]] const MacAddr& mac() const noexcept { return mac_; }
  [[nodiscard]] unsigned ifindex() const noexcept { return ifindex_; }
  [[nodiscard]] std::uint32_t mtu() const noexcept { return mtu_; }
  [[nodiscard]] const PacketRingStats& stats() const noexcept { return stats_; }

 private:
  int fd_ = -1;
  std::byte* map_ = nullptr;
  std::size_t map_len_ = 0;
  std::byte* rx_ = nullptr;
  std::byte* tx_ = nullptr;
  std::uint32_t rx_count_ = 0;
  std::uint32_t tx_count_ = 0;
  std::uint32_t rx_idx_ = 0;
  std::uint32_t tx_idx_ = 0;
  std::uint32_t tx_pending_ = 0;
  unsigned ifindex_ = 0;
  std::uint32_t mtu_ = 1500;
  MacAddr mac_{};
  PacketRingStats stats_{};
};

}  // namespace fastmm::net
