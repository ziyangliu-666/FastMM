#pragma once
// UserTcpLink: the OUCH connection over net::UserTcp (order_transport = "user_tcp"; experimental).
// The kernel's TCP stack is not involved: segments are built in user space and sent as Ethernet
// frames through one of two devices:
//   - its own net::PacketRing (AF_PACKET TX/RX rings; rx_backend = "kernel"): init()
//   - the market-data source's device (rx_backend = "af_xdp" or "dpdk"): init_shared() with the
//     source's FrameTx; the source's poll() hands the frames for the link to frame_sink()
//
// The link has an IPv4 address of its own (user_tcp_ip) on the interface's subnet and answers ARP
// for it. With the packet ring the address must not be assigned to any kernel interface (the
// kernel would answer the peer's segments with RSTs). On a shared device it may be the host's own
// address when the local port is fixed (user_tcp_port): only TCP to that port reaches the link,
// and on af_xdp the next hop's MAC then comes from the kernel's neighbour table (the kernel keeps
// the ARP traffic). The peer's segments must arrive unmerged: GRO off on a NIC, TSO/GSO off on a
// veth peer.
//
// Threading as TcpLink: everything but shutdown_from_any_thread() runs on the venue's network
// thread; poll() must be called on every loop iteration (it reads the packet ring and runs the
// retransmission timers). In adaptive spin mode the ring's descriptor is also registered with
// the reactor.
#include "fastmm/net/datagram_source.hpp"
#include "fastmm/net/packet_ring.hpp"
#include "fastmm/net/user_tcp.hpp"
#include "fastmm/venues/nasdaq/tcp_link.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace fastmm::venues::nasdaq {

struct UserTcpLinkConfig {
  std::string interface;
  std::uint32_t local_ip = 0;    // network byte order
  std::uint32_t gateway = 0;     // network byte order; 0 = the peer is on-link
  std::uint16_t local_port = 0;  // host byte order; 0 = a random port per connection
  bool register_fd = false;      // adaptive spin mode: wake the reactor on received frames
  // The kernel owns local_ip (a shared af_xdp device): the next hop's MAC comes from the kernel's
  // neighbour table instead of ARP.
  bool neighbour_mac = false;
};

class UserTcpLink final : public ByteLink, public net::IoHandler, private net::UserTcpHandler {
 public:
  UserTcpLink(TcpLinkHandler& handler, UserTcpLinkConfig cfg);
  ~UserTcpLink() override;
  UserTcpLink(const UserTcpLink&) = delete;
  UserTcpLink& operator=(const UserTcpLink&) = delete;
  UserTcpLink(UserTcpLink&&) = delete;
  UserTcpLink& operator=(UserTcpLink&&) = delete;

  // Opens the packet ring (call once, before open()). Returns 0 or -errno with `err` set.
  int init(std::string& err);
  // Or: sends through `tx` (a datagram source's device with MAC `mac`); the source must hand the
  // link's frames to frame_sink().
  void init_shared(net::FrameTx& tx, const net::MacAddr& mac, std::uint32_t mtu) noexcept;
  // The FrameSink to install on the shared source.
  [[nodiscard]] net::FrameSink frame_sink() noexcept;

  bool open(net::Reactor& reactor, const net::SockAddr& addr) noexcept override;
  void close() noexcept override;
  bool shutdown_from_any_thread() noexcept override;
  bool send(std::span<const std::byte> bytes) noexcept override;
  void cork() noexcept override;
  bool uncork() noexcept override;
  [[nodiscard]] bool connected() const noexcept override { return up_; }

  void poll() noexcept;
  // One received frame (shared device).
  void on_frame(std::span<const std::byte> frame) noexcept;

  void on_readable() override { poll(); }
  void on_writable() override {}
  void on_error(int) override {}

  [[nodiscard]] const net::UserTcpStats* tcp_stats() const noexcept {
    return tcp_ ? &tcp_->stats() : nullptr;
  }
  [[nodiscard]] const net::PacketRingStats& ring_stats() const noexcept { return ring_.stats(); }
  [[nodiscard]] bool shared() const noexcept { return tx_ != nullptr && tx_ != &ring_; }

 private:
  void on_tcp_connected() noexcept override;
  std::size_t on_tcp_data(std::span<const std::byte> bytes) noexcept override;
  void on_tcp_closed(int err) noexcept override;

  TcpLinkHandler& h_;
  UserTcpLinkConfig cfg_;
  net::PacketRing ring_;
  net::FrameTx* tx_ = nullptr;  // &ring_ or the shared device
  net::MacAddr mac_{};
  std::uint32_t mtu_ = 1500;
  std::unique_ptr<net::UserTcp> tcp_;
  std::uint32_t remote_ip_ = 0;
  std::uint16_t remote_port_ = 0;
  net::Reactor* reactor_ = nullptr;
  bool registered_ = false;
  bool up_ = false;
  bool open_ = false;
  std::atomic<bool> shutdown_{false};
};

// The MAC the kernel's neighbour table holds for `ip` (network byte order) on `interface`, after
// provoking resolution with a UDP datagram to its discard port; waits up to `timeout_ms`. False
// when it stays unresolved.
[[nodiscard]] bool kernel_neighbour_mac(const std::string& interface,
                                        std::uint32_t ip,
                                        net::MacAddr& out,
                                        int timeout_ms = 1000) noexcept;

}  // namespace fastmm::venues::nasdaq
