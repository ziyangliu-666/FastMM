#pragma once
// UserTcpLink: the OUCH connection over net::UserTcp on a net::PacketRing (order_transport =
// "user_tcp"; experimental). The kernel's TCP stack is not involved: segments are built in user
// space and written to the AF_PACKET TX ring, received segments are read from the RX ring.
//
// The link owns an IPv4 address of its own (user_tcp_ip), which must be on the interface's subnet
// and must not be assigned to any kernel interface; the link answers ARP for it. The peer's
// segments must arrive unmerged: GRO off on a NIC, TSO/GSO off on a veth peer.
//
// Threading as TcpLink: everything but shutdown_from_any_thread() runs on the venue's network
// thread; poll() must be called on every loop iteration (it reads the RX ring and runs the
// retransmission timers). In adaptive spin mode the ring's descriptor is also registered with
// the reactor.
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
  std::uint32_t local_ip = 0;  // network byte order
  std::uint32_t gateway = 0;   // network byte order; 0 = the peer is on-link
  bool register_fd = false;    // adaptive spin mode: wake the reactor on received frames
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

  bool open(net::Reactor& reactor, const net::SockAddr& addr) noexcept override;
  void close() noexcept override;
  bool shutdown_from_any_thread() noexcept override;
  bool send(std::span<const std::byte> bytes) noexcept override;
  void cork() noexcept override;
  bool uncork() noexcept override;
  [[nodiscard]] bool connected() const noexcept override { return up_; }

  void poll() noexcept;

  void on_readable() override { poll(); }
  void on_writable() override {}
  void on_error(int) override {}

  [[nodiscard]] const net::UserTcpStats* tcp_stats() const noexcept {
    return tcp_ ? &tcp_->stats() : nullptr;
  }
  [[nodiscard]] const net::PacketRingStats& ring_stats() const noexcept { return ring_.stats(); }

 private:
  void on_tcp_connected() noexcept override;
  std::size_t on_tcp_data(std::span<const std::byte> bytes) noexcept override;
  void on_tcp_closed(int err) noexcept override;

  TcpLinkHandler& h_;
  UserTcpLinkConfig cfg_;
  net::PacketRing ring_;
  std::unique_ptr<net::UserTcp> tcp_;
  std::uint32_t remote_ip_ = 0;
  std::uint16_t remote_port_ = 0;
  net::Reactor* reactor_ = nullptr;
  bool registered_ = false;
  bool up_ = false;
  bool open_ = false;
  std::atomic<bool> shutdown_{false};
};

}  // namespace fastmm::venues::nasdaq
