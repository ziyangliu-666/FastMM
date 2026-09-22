// UserTcpLink (see user_tcp_link.hpp).
#include "fastmm/venues/nasdaq/user_tcp_link.hpp"

#include "fastmm/net/reactor.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace fastmm::venues::nasdaq {

bool kernel_neighbour_mac(const std::string& interface,
                          std::uint32_t ip,
                          net::MacAddr& out,
                          int timeout_ms) noexcept {
  char want[INET_ADDRSTRLEN] = {};
  in_addr a{};
  a.s_addr = ip;
  if (::inet_ntop(AF_INET, &a, want, sizeof want) == nullptr) return false;
  const auto lookup = [&]() {
    // /proc/net/arp: "IP address  HW type  Flags  HW address  Mask  Device"
    std::FILE* f = std::fopen("/proc/net/arp", "re");
    if (f == nullptr) return false;
    char line[256];
    bool found = false;
    while (!found && std::fgets(line, sizeof line, f) != nullptr) {
      char addr[64] = {};
      char mac[64] = {};
      char dev[64] = {};
      unsigned type = 0;
      unsigned flags = 0;
      if (std::sscanf(line, "%63s 0x%x 0x%x %63s %*s %63s", addr, &type, &flags, mac, dev) != 5)
        continue;
      if (std::strcmp(addr, want) != 0 || (flags & 0x2U) == 0) continue;  // ATF_COM: complete
      if (!interface.empty() && interface != dev) continue;
      unsigned b[6] = {};
      if (std::sscanf(mac, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
        continue;
      for (int i = 0; i < 6; ++i)
        out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(b[i]);
      found = true;
    }
    std::fclose(f);
    return found;
  };
  if (lookup()) return true;
  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd >= 0) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(9);
    to.sin_addr.s_addr = ip;
    static_cast<void>(::sendto(fd, "", 0, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to));
    ::close(fd);
  }
  for (int waited = 0; waited < timeout_ms; waited += 10) {
    ::usleep(10'000);
    if (lookup()) return true;
  }
  return false;
}

UserTcpLink::UserTcpLink(TcpLinkHandler& handler, UserTcpLinkConfig cfg)
    : h_(handler), cfg_(std::move(cfg)) {}

UserTcpLink::~UserTcpLink() {
  if (registered_ && reactor_ != nullptr) reactor_->remove(ring_.fd());
}

int UserTcpLink::init(std::string& err) {
  net::PacketRingConfig pc;
  pc.interface = cfg_.interface;
  pc.local_ip = cfg_.local_ip;
  const int rc = ring_.open(pc, err);
  if (rc != 0) return rc;
  tx_ = &ring_;
  mac_ = ring_.mac();
  mtu_ = ring_.mtu();
  return 0;
}

void UserTcpLink::init_shared(net::FrameTx& tx,
                              const net::MacAddr& mac,
                              std::uint32_t mtu) noexcept {
  tx_ = &tx;
  mac_ = mac;
  mtu_ = mtu;
}

net::FrameSink UserTcpLink::frame_sink() noexcept {
  net::FrameSink s;
  s.fn = [](void* ctx, std::span<const std::byte> frame) noexcept {
    static_cast<UserTcpLink*>(ctx)->on_frame(frame);
  };
  s.ctx = this;
  s.ip = cfg_.local_ip;
  s.port = cfg_.local_port;
  return s;
}

bool UserTcpLink::open(net::Reactor& reactor, const net::SockAddr& addr) noexcept {
  if (tx_ == nullptr || addr.family() != AF_INET) return false;
  reactor_ = &reactor;
  if (cfg_.register_fd && !registered_ && ring_.fd() >= 0) {
    if (!reactor.add(ring_.fd(), *this, net::IoEvent::Read)) return false;
    registered_ = true;
  }
  const auto* in = reinterpret_cast<const sockaddr_in*>(addr.ptr());
  const std::uint32_t ip = in->sin_addr.s_addr;
  const std::uint16_t port = ntohs(in->sin_port);
  if (!tcp_ || ip != remote_ip_ || port != remote_port_) {
    net::UserTcpConfig tc;
    tc.local_mac = mac_;
    tc.local_ip = cfg_.local_ip;
    tc.local_port = cfg_.local_port;
    tc.remote_ip = ip;
    tc.remote_port = port;
    tc.next_hop_ip = cfg_.gateway;
    tc.mtu = static_cast<std::uint16_t>(std::min<std::uint32_t>(mtu_, 1500));
    if (cfg_.neighbour_mac) {
      if (!kernel_neighbour_mac(cfg_.interface, cfg_.gateway != 0 ? cfg_.gateway : ip, tc.peer_mac))
        return false;
      tc.peer_mac_static = true;
    }
    tcp_ = std::make_unique<net::UserTcp>(*tx_, static_cast<net::UserTcpHandler&>(*this), tc);
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

void UserTcpLink::on_frame(std::span<const std::byte> frame) noexcept {
  if (!tcp_) return;
  tcp_->on_frame(frame, net::Reactor::now_ns());
  tcp_->flush();
}

void UserTcpLink::poll() noexcept {
  if (!tcp_) return;
  const std::int64_t now = net::Reactor::now_ns();
  if (!shared()) {
    const std::size_t n = ring_.poll([this, now](std::span<const std::byte> f, bool unverified) {
      tcp_->on_frame(f, now, unverified);
    });
    if (n != 0) tcp_->flush();
  }
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
