// UserTcp over a PacketRing against the kernel's TCP stack: a veth pair in a user + network
// namespace, a kernel echo server on one end (10.77.0.1), the user-space client on the other
// (10.77.0.2, not assigned to any interface). Random frame loss in both directions exercises
// retransmission; the echoed stream must match byte for byte.
#include "netns_test_util.hpp"
#include "user_tcp_test_util.hpp"

#include "fastmm/net/packet_ring.hpp"
#include "fastmm/net/reactor.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <random>
#include <string>

using namespace fastmm;
using namespace fastmm::net;
using namespace fastmm::net::test;

namespace {

bool sh(const std::string& cmd) {
  return std::system((cmd + " >/dev/null 2>&1").c_str()) == 0;
}

bool have_ethtool() {
  if (sh("ethtool --version")) return true;
  MESSAGE("skipped: needs ethtool");
  return false;
}

// The kernel's segmentation offloads stay off on the server's end: AF_PACKET would see its TSO
// super-segments whole, larger than a ring frame (a NIC with GRO on needs the same).
bool make_veth() {
  return sh("ip link add utcp0 type veth peer name utcp1") && sh("ip link set utcp0 up") &&
         sh("ip addr add 10.77.0.1/24 dev utcp1") && sh("ip link set utcp1 up") &&
         sh("ethtool -K utcp1 tso off gso off");
}

// Drops a fraction of frames (seeded, reproducible) before they reach the device.
struct LossyTx final : FrameTx {
  FrameTx& inner;
  double loss;
  std::mt19937_64 rng{7};
  std::uint64_t dropped = 0;
  LossyTx(FrameTx& in, double l) : inner(in), loss(l) {}
  bool send_frame(std::span<const std::byte> f) noexcept override {
    if (std::uniform_real_distribution<double>(0, 1)(rng) < loss) {
      ++dropped;
      return true;
    }
    return inner.send_frame(f);
  }
  void flush() noexcept override { inner.flush(); }
};

int listen_on(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  REQUIRE(fd >= 0);
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = ip4("10.77.0.1");
  REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
  REQUIRE(::listen(fd, 4) == 0);
  return fd;
}

struct Rig {
  PacketRing ring;
  std::unique_ptr<LossyTx> lossy;
  RecordingHandler h;
  UserTcpConfig cfg;
  std::unique_ptr<UserTcp> tcp;
  double rx_loss = 0;
  std::mt19937_64 rng{11};
  std::uint64_t rx_dropped = 0;

  explicit Rig(double loss, std::uint16_t port) : rx_loss(loss) {
    PacketRingConfig pc;
    pc.interface = "utcp0";
    pc.local_ip = ip4("10.77.0.2");
    std::string err;
    REQUIRE_MESSAGE(ring.open(pc, err) == 0, err);
    lossy = std::make_unique<LossyTx>(ring, loss);
    cfg.local_mac = ring.mac();
    cfg.local_ip = ip4("10.77.0.2");
    cfg.remote_ip = ip4("10.77.0.1");
    cfg.remote_port = port;
    cfg.rto_min_ns = 2'000'000;
    cfg.rto_initial_ns = 20'000'000;
    cfg.max_retransmits = 30;
    cfg.syn_retries = 30;
    tcp = std::make_unique<UserTcp>(*lossy, h, cfg);
  }

  void poll() {
    const std::int64_t now = Reactor::now_ns();
    ring.poll([&](std::span<const std::byte> f, bool unverified) {
      if (std::uniform_real_distribution<double>(0, 1)(rng) < rx_loss) {
        ++rx_dropped;
        return;
      }
      tcp->on_frame(f, now, unverified);
    });
    tcp->flush();
    if (Reactor::now_ns() >= tcp->next_timer_ns()) tcp->on_timer(Reactor::now_ns());
  }
};

void echo_round(double loss) {
  REQUIRE(make_veth());
  const int lfd = listen_on(7000);
  Rig rig(loss, 7000);
  REQUIRE(rig.tcp->connect(Reactor::now_ns()));
  int cfd = -1;
  const std::int64_t deadline = Reactor::now_ns() + 60'000'000'000;
  std::string sent;
  std::string echo_pending;
  std::mt19937_64 gen(3);
  const std::size_t total = 2U << 20;  // 2 MiB each way
  while (Reactor::now_ns() < deadline) {
    rig.poll();
    if (cfd < 0) {
      cfd = ::accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    } else {
      char buf[65536];
      const ssize_t n = ::read(cfd, buf, sizeof buf);
      if (n > 0) echo_pending.append(buf, static_cast<std::size_t>(n));
      if (!echo_pending.empty()) {
        const ssize_t w = ::write(cfd, echo_pending.data(), echo_pending.size());
        if (w > 0) echo_pending.erase(0, static_cast<std::size_t>(w));
      }
    }
    if (rig.tcp->established() && sent.size() < total && rig.tcp->unacked() < (256U << 10)) {
      std::string chunk(1 + gen() % 3000, '\0');
      for (char& c : chunk) c = static_cast<char>(gen());
      chunk.resize(std::min(chunk.size(), total - sent.size()));
      REQUIRE(rig.tcp->send(bytes_of(chunk)));
      sent += chunk;
    }
    if (sent.size() == total && rig.h.data.size() == total) break;
    REQUIRE(rig.h.closed == -1);
  }
  INFO("loss " << loss << " sent " << sent.size() << " echoed " << rig.h.data.size()
               << " retransmits " << rig.tcp->stats().retransmits << " out_of_order "
               << rig.tcp->stats().out_of_order << " state " << to_string(rig.tcp->state()));
  REQUIRE(rig.h.data.size() == total);
  CHECK(rig.h.data == sent);
  if (loss > 0) CHECK(rig.tcp->stats().retransmits > 0);
  CHECK(rig.tcp->stats().bad_frames == 0);
  MESSAGE("loss " << loss << ": retransmits " << rig.tcp->stats().retransmits << ", rto "
                  << rig.tcp->stats().rto_expiries << ", fast " << rig.tcp->stats().fast_retransmits
                  << ", out of order " << rig.tcp->stats().out_of_order << ", srtt "
                  << rig.tcp->stats().srtt_ns << " ns");

  // Graceful close from our side: the server reads EOF and closes; we pass TIME_WAIT.
  rig.tcp->close();
  bool eof = false;
  const std::int64_t close_deadline = Reactor::now_ns() + 20'000'000'000;
  while (Reactor::now_ns() < close_deadline) {
    rig.poll();
    if (!eof) {
      char buf[4096];
      const ssize_t n = ::read(cfd, buf, sizeof buf);
      if (n == 0) {
        eof = true;
        ::close(cfd);
      }
    }
    if (eof && (rig.tcp->state() == TcpState::TimeWait || rig.tcp->state() == TcpState::Closed))
      break;
  }
  CHECK(eof);
  CHECK((rig.tcp->state() == TcpState::TimeWait || rig.tcp->state() == TcpState::Closed));
  ::close(lfd);
}

}  // namespace

TEST_CASE("UserTcp veth: echo through the kernel TCP stack without loss") {
  if (!in_multicast_netns() || !have_ethtool()) return;
  echo_round(0.0);
}

TEST_CASE("UserTcp veth: echo through the kernel TCP stack with 3 percent loss each way") {
  if (!in_multicast_netns() || !have_ethtool()) return;
  echo_round(0.03);
}

TEST_CASE("UserTcp veth: the server closing reports EOF; a closed port refuses") {
  if (!in_multicast_netns() || !have_ethtool()) return;
  REQUIRE(make_veth());
  const int lfd = listen_on(7001);
  Rig rig(0.0, 7001);
  REQUIRE(rig.tcp->connect(Reactor::now_ns()));
  int cfd = -1;
  const std::int64_t deadline = Reactor::now_ns() + 10'000'000'000;
  while (Reactor::now_ns() < deadline && cfd < 0) {
    rig.poll();
    cfd = ::accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
  }
  REQUIRE(cfd >= 0);
  while (Reactor::now_ns() < deadline && !rig.h.connected) rig.poll();
  REQUIRE(rig.h.connected);
  REQUIRE(::write(cfd, "bye", 3) == 3);
  ::close(cfd);
  while (Reactor::now_ns() < deadline && rig.h.closed == -1) rig.poll();
  CHECK(rig.h.data == "bye");
  CHECK(rig.h.closed == 0);
  while (Reactor::now_ns() < deadline && rig.tcp->state() != TcpState::Closed) rig.poll();
  CHECK(rig.tcp->state() == TcpState::Closed);

  // A port nobody listens on: the kernel answers the SYN with RST.
  rig.h.closed = -1;
  rig.cfg.remote_port = 7002;
  rig.tcp = std::make_unique<UserTcp>(*rig.lossy, rig.h, rig.cfg);
  REQUIRE(rig.tcp->connect(Reactor::now_ns()));
  while (Reactor::now_ns() < deadline && rig.h.closed == -1) rig.poll();
  CHECK(rig.h.closed == ECONNREFUSED);

  // The server aborts (SO_LINGER 0): RST.
  rig.h = RecordingHandler{};
  rig.cfg.remote_port = 7001;
  rig.tcp = std::make_unique<UserTcp>(*rig.lossy, rig.h, rig.cfg);
  REQUIRE(rig.tcp->connect(Reactor::now_ns()));
  cfd = -1;
  while (Reactor::now_ns() < deadline && (cfd < 0 || !rig.h.connected)) {
    rig.poll();
    if (cfd < 0) cfd = ::accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
  }
  REQUIRE(cfd >= 0);
  linger lg{1, 0};
  ::setsockopt(cfd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
  ::close(cfd);
  while (Reactor::now_ns() < deadline && rig.h.closed == -1) rig.poll();
  CHECK(rig.h.closed == ECONNRESET);
  ::close(lfd);
}
