// UserTcpLink (see user_tcp_link.hpp).
#include "fastmm/venues/nasdaq/user_tcp_link.hpp"

#include "fastmm/net/reactor.hpp"

#include <netinet/in.h>

#include <cerrno>

namespace fastmm::venues::nasdaq {

UserTcpLink::UserTcpLink(TcpLinkHandler& handler, UserTcpLinkConfig cfg)
    : h_(handler), cfg_(std::move(cfg)) {}

UserTcpLink::~UserTcpLink() {
  if (registered_ && reactor_ != nullptr) reactor_->remove(ring_.fd());
}

int UserTcpLink::init(std::string& err) {
  net::PacketRingConfig pc;
  pc.interface = cfg_.interface;
  pc.local_ip = cfg_.local_ip;
  return ring_.open(pc, err);
}

bool UserTcpLink::open(net::Reactor& reactor, const net::SockAddr& addr) noexcept {
  if (ring_.fd() < 0 || addr.family() != AF_INET) return false;
  reactor_ = &reactor;
  if (cfg_.register_fd && !registered_) {
    if (!reactor.add(ring_.fd(), *this, net::IoEvent::Read)) return false;
    registered_ = true;
  }
  const auto* in = reinterpret_cast<const sockaddr_in*>(addr.ptr());
  const std::uint32_t ip = in->sin_addr.s_addr;
  const std::uint16_t port = ntohs(in->sin_port);
  if (!tcp_ || ip != remote_ip_ || port != remote_port_) {
    net::UserTcpConfig tc;
    tc.local_mac = ring_.mac();
    tc.local_ip = cfg_.local_ip;
    tc.remote_ip = ip;
    tc.remote_port = port;
    tc.next_hop_ip = cfg_.gateway;
    tc.mtu = static_cast<std::uint16_t>(std::min<std::uint32_t>(ring_.mtu(), 1500));
    tcp_ = std::make_unique<net::UserTcp>(ring_, static_cast<net::UserTcpHandler&>(*this), tc);
    remote_ip_ = ip;
    remote_port_ = port;
  }
  up_ = false;
  open_ = true;
  shutdown_.store(false, std::memory_order_relaxed);
  return tcp_->connect(net::Reactor::now_ns());
}

void UserTcpLink::close() noexcept {
  if (!open_) return;
  open_ = false;
  up_ = false;
  // A FIN after what is queued (as close(2) on a socket whose data was read); the engine keeps
  // answering the peer until the close completes, without callbacks.
  if (tcp_) tcp_->close();
}

bool UserTcpLink::shutdown_from_any_thread() noexcept {
  shutdown_.store(true, std::memory_order_release);
  return true;
}

bool UserTcpLink::send(std::span<const std::byte> bytes) noexcept {
  if (!up_) return false;
  return tcp_->send(bytes);
}

void UserTcpLink::cork() noexcept {
  if (tcp_) tcp_->cork();
}

bool UserTcpLink::uncork() noexcept {
  if (!tcp_) return false;
  if (!up_) {
    static_cast<void>(tcp_->uncork());
    return false;
  }
  return tcp_->uncork();
}

void UserTcpLink::poll() noexcept {
  if (!tcp_) return;
  const std::int64_t now = net::Reactor::now_ns();
  const std::size_t n = ring_.poll([this, now](std::span<const std::byte> f, bool unverified) {
    tcp_->on_frame(f, now, unverified);
  });
  if (n != 0) tcp_->flush();
  if (now >= tcp_->next_timer_ns()) tcp_->on_timer(now);
  if (shutdown_.load(std::memory_order_relaxed) && open_ &&
      shutdown_.exchange(false, std::memory_order_acquire)) {
    // cancel_all: the connection ends as a kernel socket's would after SHUT_RDWR.
    close();
    h_.on_link_down(0);
  }
}

void UserTcpLink::on_tcp_connected() noexcept {
  if (!open_) return;
  up_ = true;
  h_.on_link_up();
}

std::size_t UserTcpLink::on_tcp_data(std::span<const std::byte> bytes) noexcept {
  if (!open_) return bytes.size();
  return h_.on_link_data(bytes);
}

void UserTcpLink::on_tcp_closed(int err) noexcept {
  if (!open_) return;
  open_ = false;
  up_ = false;
  h_.on_link_down(err);
}

}  // namespace fastmm::venues::nasdaq
