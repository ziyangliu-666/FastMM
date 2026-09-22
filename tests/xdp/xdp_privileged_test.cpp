// AF_XDP tests that need privileges: the kernel verifier, BPF_PROG_TEST_RUN and receive over a
// veth pair. Without them each case passes with MESSAGE("skipped: ..."); with
// FASTMM_XDP_REQUIRE=1 (set by scripts/xdp-test.sh) a skip is a failure.
//
// The veth cases need root: they create two network namespaces with `ip netns add`, a veth pair
// between them, and switch this thread into each with setns(2) to create the sockets there.
#include "../net/user_tcp_test_util.hpp"
#include "test_support.hpp"
#include "xdp_test_util.hpp"

#include "fastmm/net/reactor.hpp"
#include "fastmm/net/udp_frame.hpp"
#include "fastmm/net/user_tcp.hpp"
#include "fastmm/net/xdp_datagram_source.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace fastmm::net;
using fastmm::net::test::build_frame;
using fastmm::net::test::FrameSpec;
using fastmm::net::test::ip;

namespace {

bool require_privileged() {
  const char* v = std::getenv("FASTMM_XDP_REQUIRE");
  return v != nullptr && std::string(v) == "1";
}

// "" when the BPF and AF_XDP cases can run.
std::string bpf_skip_reason() {
  if (std::string e = xdp_kernel_error(); !e.empty()) return e;
  return xdp_capability_error();
}

// "" when the veth cases can run.
std::string veth_skip_reason() {
  if (std::string e = bpf_skip_reason(); !e.empty()) return e;
  if (::geteuid() != 0) return "needs root (ip netns add, setns)";
  if (std::system("ip -V >/dev/null 2>&1") != 0) return "needs iproute2 (`ip`)";
  return {};
}

#define FASTMM_XDP_SKIP_UNLESS(reason_expr)               \
  do {                                                    \
    const std::string fastmm_skip_reason = (reason_expr); \
    if (!fastmm_skip_reason.empty()) {                    \
      if (require_privileged()) FAIL(fastmm_skip_reason); \
      MESSAGE("skipped: " << fastmm_skip_reason);         \
      return;                                             \
    }                                                     \
  } while (0)

bool sh(const std::string& cmd) {
  return std::system((cmd + " >/dev/null 2>&1").c_str()) == 0;
}

// Two namespaces joined by a veth pair: `rx_if` (10.203.0.2/24) in the receiver namespace and
// `tx_if` (10.203.0.1/24, multicast route) in the sender namespace. Destroyed with the namespaces.
struct VethPair {
  std::string rx_ns = "fxdp_r" + std::to_string(::getpid());
  std::string tx_ns = "fxdp_s" + std::to_string(::getpid());
  std::string rx_if = "fxr" + std::to_string(::getpid() % 100000);
  std::string tx_if = "fxs" + std::to_string(::getpid() % 100000);
  int home_ns = -1;
  bool ok = false;

  // `queues`: RX and TX queues on each end.
  explicit VethPair(unsigned queues = 1) {
    teardown();
    const std::string q = queues == 1 ? std::string()
                                      : " numrxqueues " + std::to_string(queues) + " numtxqueues " +
                                            std::to_string(queues);
    home_ns = ::open("/proc/thread-self/ns/net", O_RDONLY | O_CLOEXEC);
    const std::string r = "ip -n " + rx_ns + " ";
    const std::string t = "ip -n " + tx_ns + " ";
    ok = home_ns >= 0 && sh("ip netns add " + rx_ns) && sh("ip netns add " + tx_ns) &&
         sh(r + "link add " + rx_if + q + " type veth peer name " + tx_if + q) &&
         sh(r + "link set " + tx_if + " netns " + tx_ns) &&
         sh(r + "addr add 10.203.0.2/24 dev " + rx_if) && sh(r + "link set lo up") &&
         sh(r + "link set " + rx_if + " up") && sh(t + "addr add 10.203.0.1/24 dev " + tx_if) &&
         sh(t + "link set lo up") && sh(t + "link set " + tx_if + " up") &&
         sh(t + "route add 224.0.0.0/4 dev " + tx_if);
  }
  ~VethPair() {
    go_home();
    if (home_ns >= 0) ::close(home_ns);
    teardown();
  }
  VethPair(const VethPair&) = delete;
  VethPair& operator=(const VethPair&) = delete;

  void teardown() const {
    sh("ip netns del " + rx_ns);
    sh("ip netns del " + tx_ns);
  }
  static bool enter(const std::string& ns) {
    const std::string path = "/var/run/netns/" + ns;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool r = ::setns(fd, CLONE_NEWNET) == 0;
    ::close(fd);
    return r;
  }
  bool enter_rx() const { return enter(rx_ns); }
  bool enter_tx() const { return enter(tx_ns); }
  void go_home() const {
    if (home_ns >= 0) ::setns(home_ns, CLONE_NEWNET);
  }
};

int udp_socket() {
  return ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
}

// veth offloads the UDP checksum: frames leave with only the pseudo-header sum filled in, and the
// user-space check would reject them. Turning TX checksumming off on the sender makes the stack
// compute it, so verify_udp_checksum can be tested over veth.
bool disable_tx_checksum(int fd, const std::string& ifname) {
  ethtool_value v{};
  v.cmd = ETHTOOL_STXCSUM;
  v.data = 0;
  ifreq ifr{};
  std::strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
  ifr.ifr_data = reinterpret_cast<char*>(&v);
  return ::ioctl(fd, SIOCETHTOOL, &ifr) == 0;
}

// Kernel receiver bound to `port` on `bind_ip` (network order), joined to `group` on `ifname`
// when group != 0.
int kernel_receiver(std::uint32_t bind_ip,
                    std::uint16_t port,
                    std::uint32_t group,
                    const std::string& ifname) {
  const int fd = udp_socket();
  if (fd < 0) return -1;
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = bind_ip;
  if (::bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0) {
    ::close(fd);
    return -1;
  }
  if (group != 0) {
    ip_mreqn m{};
    m.imr_multiaddr.s_addr = group;
    m.imr_ifindex = static_cast<int>(::if_nametoindex(ifname.c_str()));
    if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) != 0) {
      ::close(fd);
      return -1;
    }
  }
  return fd;
}

bool send_to(int fd, std::uint32_t dst, std::uint16_t port, const std::string& payload) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = dst;
  return ::sendto(fd,
                  payload.data(),
                  payload.size(),
                  0,
                  reinterpret_cast<const sockaddr*>(&a),
                  sizeof(a)) == static_cast<ssize_t>(payload.size());
}

struct Received {
  std::string payload;
  RxMeta meta;
};

constexpr std::uint16_t kPortA = 31001;
constexpr std::uint16_t kPortB = 31002;
constexpr std::uint16_t kPortKernel = 31003;
constexpr std::uint16_t kPortUnicast = 31004;
constexpr int kPerLine = 200;

template <class Pred>
bool wait_until(Pred pred, int ms = 5000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  return true;
}

// Receives on the veth pair in `mode` and checks payloads, lines, stats, the attached program's
// verdicts, and that traffic the program does not claim still reaches kernel sockets.
void veth_receive(XdpMode mode, XdpMode expect) {
  FASTMM_XDP_SKIP_UNLESS(veth_skip_reason());
  VethPair net;
  REQUIRE_MESSAGE(net.ok, "veth setup failed (ip netns / ip link)");

  // Sender side.
  REQUIRE(net.enter_tx());
  const int tx = udp_socket();
  REQUIRE(tx >= 0);
  REQUIRE(disable_tx_checksum(tx, net.tx_if));
  ip_mreqn mif{};
  mif.imr_ifindex = static_cast<int>(::if_nametoindex(net.tx_if.c_str()));
  REQUIRE(::setsockopt(tx, IPPROTO_IP, IP_MULTICAST_IF, &mif, sizeof(mif)) == 0);

  // Receiver side.
  REQUIRE(net.enter_rx());
  const int k_mcast = kernel_receiver(INADDR_ANY, kPortKernel, ip("239.10.0.3"), net.rx_if);
  const int k_ucast = kernel_receiver(ip("10.203.0.2"), kPortUnicast, 0, net.rx_if);
  REQUIRE(k_mcast >= 0);
  REQUIRE(k_ucast >= 0);
  XdpDatagramSource src;
  XdpConfig cfg;
  cfg.subscriptions.push_back({net.rx_if, ip("239.10.0.1"), kPortA, 0});
  cfg.subscriptions.push_back({net.rx_if, ip("239.10.0.2"), kPortB, 0});
  cfg.frame_count = 256;
  cfg.batch = 16;
  cfg.mode = mode;
  cfg.verify_udp_checksum = true;
  const int rc = src.open(cfg);
  net.go_home();
  REQUIRE_MESSAGE(rc == 0, src.error());
  REQUIRE(src.interfaces().size() == 1);
  CHECK(src.interfaces()[0].mode == expect);
  MESSAGE("mode " << to_string(src.interfaces()[0].mode));
  REQUIRE(src.fds().size() == 1);

  std::vector<Received> got;
  got.reserve(2 * kPerLine);
  // Poll until empty: one poll takes at most `batch` frames, and a burst larger than the UMEM
  // runs the fill ring dry unless frames go back as fast as they arrive.
  const auto drain = [&] {
    while (src.poll([&](std::span<const std::byte> p, const RxMeta& m) noexcept {
      got.push_back({std::string(reinterpret_cast<const char*>(p.data()), p.size()), m});
    }) != 0) {
    }
  };
  for (int i = 0; i < kPerLine; ++i) {
    REQUIRE(send_to(tx, ip("239.10.0.1"), kPortA, "A" + std::to_string(i)));
    REQUIRE(send_to(tx, ip("239.10.0.2"), kPortB, "B" + std::to_string(i)));
    if (i % 16 == 0) drain();
  }
  CHECK(wait_until([&] {
    drain();
    return got.size() >= 2U * kPerLine;
  }));
  src.refresh_stats();
  MESSAGE("datagrams " << src.stats().datagrams << ", bad frames " << src.stats().bad_frames
                       << ", unmatched " << src.stats().unmatched << ", fill short "
                       << src.stats().fill_short << ", fallback "
                       << src.interfaces()[0].fallback_packets << ", rx ring full "
                       << src.socket_stats()[0].rx_ring_full << ", fill ring empty "
                       << src.socket_stats()[0].rx_fill_ring_empty_descs);
  CHECK(src.stats().fill_short == 0);
  REQUIRE(got.size() == 2U * kPerLine);
  int next_a = 0;
  int next_b = 0;
  for (const auto& r : got) {
    CHECK(r.meta.src_ip == ip("10.203.0.1"));
    CHECK(r.meta.t0_cycles.v != 0);
    CHECK(r.meta.t0_wall_ns > 0);
    CHECK(r.meta.sw_ts_ns == 0);
    CHECK(r.meta.hw_ts_ns == 0);
    if (r.meta.line == 0) {
      CHECK(r.meta.dst_ip == ip("239.10.0.1"));
      CHECK(r.meta.dst_port == kPortA);
      CHECK(r.payload == "A" + std::to_string(next_a++));  // one line keeps its order
    } else {
      CHECK(r.meta.line == 1);
      CHECK(r.meta.dst_ip == ip("239.10.0.2"));
      CHECK(r.meta.dst_port == kPortB);
      CHECK(r.payload == "B" + std::to_string(next_b++));
    }
  }
  CHECK(next_a == kPerLine);
  CHECK(next_b == kPerLine);
  CHECK(src.stats().datagrams == 2U * kPerLine);
  CHECK(src.stats().bad_frames == 0);
  CHECK(src.stats().unmatched == 0);
  CHECK(src.stats().fill_short == 0);

  // Not subscribed: an unsubscribed group and unicast reach the kernel sockets; ARP for the
  // unicast destination passes through the program too.
  REQUIRE(send_to(tx, ip("239.10.0.3"), kPortKernel, "kernel-mcast"));
  REQUIRE(send_to(tx, ip("10.203.0.2"), kPortUnicast, "kernel-ucast"));
  char buf[64];
  ssize_t n_m = -1;
  ssize_t n_u = -1;
  CHECK(wait_until([&] {
    drain();
    if (n_m < 0) n_m = ::recv(k_mcast, buf, sizeof(buf), 0);
    if (n_u < 0) n_u = ::recv(k_ucast, buf, sizeof(buf), 0);
    return n_m > 0 && n_u > 0;
  }));
  CHECK(n_m == static_cast<ssize_t>(std::strlen("kernel-mcast")));
  CHECK(n_u == static_cast<ssize_t>(std::strlen("kernel-ucast")));
  CHECK(got.size() == 2U * kPerLine);

  REQUIRE(src.refresh_stats() == 0);
  CHECK(src.interfaces()[0].fallback_packets == 0);
  REQUIRE(src.socket_stats().size() == 1);
  const XdpSocketStats& ss = src.socket_stats()[0];
  CHECK(ss.queue == 0);
  CHECK(ss.rx_invalid_descs == 0);
  CHECK(ss.rx_ring_full == 0);
  MESSAGE("rx_dropped " << ss.rx_dropped << ", rx_fill_ring_empty_descs "
                        << ss.rx_fill_ring_empty_descs);

  // The attached program redirects a subscribed frame (queue 0 has the socket) and passes the
  // others; BPF_PROG_TEST_RUN does not deliver, it only reports the action.
  std::uint32_t action = 0;
  REQUIRE(src.filter(0).test_run(build_frame(FrameSpec{}), action) == 0);
  CHECK(action == static_cast<std::uint32_t>(xdp::kRedirect));
  FrameSpec other;
  other.dst_ip = ip("239.10.0.3");
  REQUIRE(src.filter(0).test_run(build_frame(other), action) == 0);
  CHECK(action == static_cast<std::uint32_t>(xdp::kPass));

  ::close(tx);
  ::close(k_mcast);
  ::close(k_ucast);
}

// UserTcp on the XDP socket (tcp_* in XdpConfig) against a kernel TCP echo server in the sender
// namespace, with a unicast line on the receiver's address. shared_ip: the stack uses the
// receiver's own address (the kernel keeps ARP; fixed port, the peer's MAC given).
void veth_user_tcp(XdpMode mode, bool shared_ip) {
  FASTMM_XDP_SKIP_UNLESS(veth_skip_reason());
  if (std::system("ethtool --version >/dev/null 2>&1") != 0) {
    if (require_privileged()) FAIL("needs ethtool");
    MESSAGE("skipped: needs ethtool");
    return;
  }
  VethPair net;
  REQUIRE_MESSAGE(net.ok, "veth setup failed (ip netns / ip link)");
  REQUIRE(
      sh("ip netns exec " + net.tx_ns + " ethtool -K " + net.tx_if + " tso off gso off tx off"));

  // Sender side: the echo server, a UDP sender, and its MAC.
  REQUIRE(net.enter_tx());
  const int lfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  REQUIRE(lfd >= 0);
  sockaddr_in la{};
  la.sin_family = AF_INET;
  la.sin_port = htons(7000);
  la.sin_addr.s_addr = ip("10.203.0.1");
  REQUIRE(::bind(lfd, reinterpret_cast<const sockaddr*>(&la), sizeof la) == 0);
  REQUIRE(::listen(lfd, 4) == 0);
  const int tx = udp_socket();
  REQUIRE(tx >= 0);
  MacAddr peer_mac{};
  {
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, net.tx_if.c_str(), IFNAMSIZ - 1);
    REQUIRE(::ioctl(tx, SIOCGIFHWADDR, &ifr) == 0);
    std::memcpy(peer_mac.data(), ifr.ifr_hwaddr.sa_data, 6);
  }

  // Receiver side.
  REQUIRE(net.enter_rx());
  const std::uint32_t tcp_ip = shared_ip ? ip("10.203.0.2") : ip("10.203.0.5");
  XdpDatagramSource src;
  XdpConfig cfg;
  cfg.subscriptions.push_back({net.rx_if, ip("10.203.0.2"), kPortA, 0});  // unicast line
  cfg.frame_count = 256;
  cfg.batch = 16;
  cfg.mode = mode;
  cfg.tcp_interface = net.rx_if;
  cfg.tcp_ip = tcp_ip;
  cfg.tcp_port = shared_ip ? 7100 : 0;
  cfg.tcp_arp = !shared_ip;
  cfg.tx_frames = 64;
  const int rc = src.open(cfg);
  net.go_home();
  REQUIRE_MESSAGE(rc == 0, src.error());
  REQUIRE(src.has_tx());

  test::RecordingHandler h;
  UserTcpConfig tc;
  tc.local_mac = src.mac();
  tc.local_ip = tcp_ip;
  tc.local_port = shared_ip ? 7100 : 0;
  tc.remote_ip = ip("10.203.0.1");
  tc.remote_port = 7000;
  tc.rto_min_ns = 2'000'000;
  tc.rto_initial_ns = 20'000'000;
  tc.peer_mac = peer_mac;
  tc.peer_mac_static = shared_ip;
  UserTcp tcp(src.frame_tx(), h, tc);
  FrameSink sink;
  sink.fn = [](void* p, std::span<const std::byte> f) noexcept {
    auto* t = static_cast<UserTcp*>(p);
    t->on_frame(f, Reactor::now_ns());
    t->flush();
  };
  sink.ctx = &tcp;
  sink.ip = tcp_ip;
  sink.port = tc.local_port;
  src.set_frame_sink(sink);
  REQUIRE(tcp.connect(Reactor::now_ns()));

  std::vector<std::string> datagrams;
  const auto poll = [&] {
    src.poll([&](std::span<const std::byte> p, const RxMeta& m) noexcept {
      datagrams.emplace_back(reinterpret_cast<const char*>(p.data()), p.size());
      CHECK(m.dst_ip == ip("10.203.0.2"));
    });
    if (Reactor::now_ns() >= tcp.next_timer_ns()) tcp.on_timer(Reactor::now_ns());
  };
  int cfd = -1;
  std::string sent;
  std::string pending;
  std::mt19937_64 gen(9);
  const std::size_t total = 256U << 10;
  int udp_sent = 0;
  const std::int64_t deadline = Reactor::now_ns() + 30'000'000'000;
  while (Reactor::now_ns() < deadline) {
    poll();
    if (udp_sent < kPerLine)
      REQUIRE(send_to(tx, ip("10.203.0.2"), kPortA, "U" + std::to_string(udp_sent++)));
    if (cfd < 0) {
      cfd = ::accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    } else {
      char buf[65536];
      const ssize_t r = ::read(cfd, buf, sizeof buf);
      if (r > 0) pending.append(buf, static_cast<std::size_t>(r));
      if (!pending.empty()) {
        const ssize_t w = ::write(cfd, pending.data(), pending.size());
        if (w > 0) pending.erase(0, static_cast<std::size_t>(w));
      }
    }
    if (tcp.established() && sent.size() < total && tcp.unacked() < (64U << 10)) {
      std::string chunk(1 + gen() % 1400, '\0');
      for (char& ch : chunk) ch = static_cast<char>(gen());
      chunk.resize(std::min(chunk.size(), total - sent.size()));
      REQUIRE(tcp.send(test::bytes_of(chunk)));
      sent += chunk;
    }
    if (sent.size() == total && h.data.size() == total &&
        datagrams.size() == static_cast<std::size_t>(kPerLine))
      break;
    REQUIRE(h.closed == -1);
  }
  MESSAGE("mode " << to_string(src.interfaces()[0].mode) << (shared_ip ? ", shared address" : "")
                  << ": to_sink " << src.stats().to_sink << ", tx " << src.stats().tx_frames
                  << ", tx drops " << src.stats().tx_drops << ", kicks " << src.stats().tx_kicks
                  << ", retransmits " << tcp.stats().retransmits);
  REQUIRE(h.data.size() == total);
  CHECK(h.data == sent);
  CHECK(tcp.stats().bad_frames == 0);
  CHECK(src.stats().tx_drops == 0);
  REQUIRE(datagrams.size() == static_cast<std::size_t>(kPerLine));
  for (int i = 0; i < kPerLine; ++i)
    CHECK(datagrams[static_cast<std::size_t>(i)] == "U" + std::to_string(i));
  tcp.abort();
  if (cfd >= 0) ::close(cfd);
  ::close(lfd);
  ::close(tx);
}

}  // namespace

TEST_CASE("veth: UserTcp on the XDP socket and a unicast line in generic mode") {
  veth_user_tcp(XdpMode::Generic, false);
}

TEST_CASE("veth: UserTcp on the XDP socket and a unicast line in native copy mode") {
  veth_user_tcp(XdpMode::NativeCopy, false);
}

TEST_CASE("veth: UserTcp on the XDP socket with the host's own address and a fixed port") {
  veth_user_tcp(XdpMode::NativeCopy, true);
}

TEST_CASE("bpf: the verifier accepts the filter program") {
  FASTMM_XDP_SKIP_UNLESS(bpf_skip_reason());
  XdpFilter f;
  std::string err;
  const auto keys = test::case_keys();
  const int rc = f.create(keys, 4, err);
  REQUIRE_MESSAGE(rc == 0, err);
  CHECK(f.prog_fd() >= 0);
  std::uint64_t fallback = 1;
  REQUIRE(f.fallback_count(fallback) == 0);
  CHECK(fallback == 0);
  // With the UserTcp rules (address and port, with and without ARP).
  for (const bool arp : {true, false}) {
    XdpFilter g;
    const int rc2 = g.create(keys, 4, err, {ip("10.203.0.200"), 7000, arp});
    REQUIRE_MESSAGE(rc2 == 0, err);
    CHECK(g.prog_fd() >= 0);
  }
}

TEST_CASE("bpf: BPF_PROG_TEST_RUN verdicts agree with the parser") {
  FASTMM_XDP_SKIP_UNLESS(bpf_skip_reason());
  XdpFilter f;
  std::string err;
  const auto keys = test::case_keys();
  REQUIRE_MESSAGE(f.create(keys, 1, err) == 0, err);
  // No socket in the XSKMAP: subscribed frames come back XDP_PASS and count as fallbacks.
  std::size_t matched = 0;
  for (const auto& c : test::frame_cases()) {
    for (std::size_t n = 14; n <= c.frame.size(); ++n) {
      const auto frame = std::span<const std::byte>(c.frame).first(n);
      const UdpFrame parsed = parse_udp_frame(frame);
      const bool match = passes_xdp_filter(parsed.status) && test::key_matches(parsed);
      INFO(std::string(c.name) << ", " << n << " of " << c.frame.size() << " bytes");
      std::uint64_t before = 0;
      std::uint64_t after = 0;
      REQUIRE(f.fallback_count(before) == 0);
      std::uint32_t action = 0;
      const int rc = f.test_run(frame, action);
      REQUIRE_MESSAGE(rc == 0, std::strerror(-rc));
      REQUIRE(f.fallback_count(after) == 0);
      CHECK(action == static_cast<std::uint32_t>(xdp::kPass));
      CHECK(after - before == (match ? 1U : 0U));
      matched += match ? 1 : 0;
    }
  }
  CHECK(matched > 10);
}

TEST_CASE("veth: receive in generic mode") {
  veth_receive(XdpMode::Generic, XdpMode::Generic);
}

TEST_CASE("veth: receive in native copy mode") {
  veth_receive(XdpMode::NativeCopy, XdpMode::NativeCopy);
}

TEST_CASE("veth: auto settles on native copy (veth has no zero-copy)") {
  veth_receive(XdpMode::Auto, XdpMode::NativeCopy);
}

// Unlisted queues: a socket on every RX queue the interface has once the program is attached
// (virtio_net adds queues with XDP and the host delivers on all of them). Datagrams from many
// flows, which the sender spreads over its TX queues and so over the receiver's RX queues, all
// arrive and none falls back to the kernel.
TEST_CASE("veth: a socket on every RX queue unless the queues are listed") {
  FASTMM_XDP_SKIP_UNLESS(veth_skip_reason());
  constexpr unsigned kQueues = 4;
  constexpr int kFlows = 32;
  constexpr int kPerFlow = 4;
  VethPair net(kQueues);
  REQUIRE_MESSAGE(net.ok, "veth setup failed (ip netns / ip link)");
  REQUIRE(net.enter_tx());
  std::vector<int> tx;
  for (int i = 0; i < kFlows; ++i) {
    tx.push_back(udp_socket());
    REQUIRE(tx.back() >= 0);
  }
  REQUIRE(net.enter_rx());
  XdpConfig cfg;
  cfg.subscriptions.push_back({net.rx_if, ip("10.203.0.2"), kPortUnicast, 0});
  cfg.frame_count = 256;
  cfg.mode = XdpMode::NativeCopy;
  XdpDatagramSource src;
  int rc = src.open(cfg);
  net.go_home();
  REQUIRE_MESSAGE(rc == 0, src.error());
  REQUIRE(src.fds().size() == kQueues);
  for (unsigned q = 0; q < kQueues; ++q) CHECK(src.socket_stats()[q].queue == q);

  std::size_t got = 0;
  const auto drain = [&] {
    while (src.poll([&](std::span<const std::byte>, const RxMeta&) noexcept { ++got; }) != 0) {
    }
  };
  for (int i = 0; i < kPerFlow; ++i) {
    for (const int fd : tx) REQUIRE(send_to(fd, ip("10.203.0.2"), kPortUnicast, "x"));
    drain();
  }
  CHECK(wait_until([&] {
    drain();
    return got >= static_cast<std::size_t>(kFlows * kPerFlow);
  }));
  CHECK(got == static_cast<std::size_t>(kFlows * kPerFlow));
  REQUIRE(src.refresh_stats() == 0);
  CHECK(src.interfaces()[0].fallback_packets == 0);
  src.close();

  cfg.queues.push_back({net.rx_if, {2}});
  REQUIRE(net.enter_rx());
  XdpDatagramSource listed;
  rc = -EBUSY;
  CHECK(wait_until(  // the previous sockets' bindings clear from a workqueue
      [&] {
        rc = listed.open(cfg);
        return rc != -EBUSY;
      },
      2000));
  net.go_home();
  REQUIRE_MESSAGE(rc == 0, listed.error());
  REQUIRE(listed.fds().size() == 1);
  CHECK(listed.socket_stats()[0].queue == 2);
  for (const int fd : tx) ::close(fd);
}

TEST_CASE("veth: zero-copy fails cleanly and a second program is refused with EBUSY") {
  FASTMM_XDP_SKIP_UNLESS(veth_skip_reason());
  VethPair net;
  REQUIRE_MESSAGE(net.ok, "veth setup failed (ip netns / ip link)");
  REQUIRE(net.enter_rx());
  XdpConfig cfg;
  cfg.subscriptions.push_back({net.rx_if, ip("239.10.0.1"), kPortA, 0});
  cfg.frame_count = 128;
  cfg.mode = XdpMode::ZeroCopy;
  XdpDatagramSource zc;
  CHECK(zc.open(cfg) < 0);
  CHECK(zc.error().find("zerocopy") != std::string::npos);
  MESSAGE(zc.error());

  // Another program already attached (here in generic mode, without sockets): every mode fails
  // with EBUSY, the native attempts because generic and native XDP cannot coexist.
  {
    XdpFilter other;
    std::string err;
    const auto keys = test::case_keys();
    REQUIRE_MESSAGE(other.create(keys, 1, err) == 0, err);
    REQUIRE(other.attach(::if_nametoindex(net.rx_if.c_str()), true) == 0);
    cfg.mode = XdpMode::Auto;
    XdpDatagramSource busy;
    CHECK(busy.open(cfg) == -EBUSY);
    CHECK(busy.error().find("already has an XDP program") != std::string::npos);
    MESSAGE(busy.error());
    cfg.mode = XdpMode::Generic;
    CHECK(busy.open(cfg) == -EBUSY);
  }

  // The failed attempts left nothing attached; a second source on the same queue fails to bind.
  cfg.mode = XdpMode::Generic;
  XdpDatagramSource first;
  CHECK_MESSAGE(first.open(cfg) == 0, first.error());
  XdpDatagramSource second;
  CHECK(second.open(cfg) == -EBUSY);
  MESSAGE(second.error());
  first.close();
  // The kernel clears the queue's socket binding from a workqueue after close, so a new bind on
  // the same queue can see EBUSY for a moment.
  XdpDatagramSource third;
  int rc = -EBUSY;
  CHECK(wait_until(
      [&] {
        rc = third.open(cfg);
        return rc != -EBUSY;
      },
      2000));
  CHECK_MESSAGE(rc == 0, third.error());  // closing the link fd detached the program
  net.go_home();
}

TEST_CASE("veth: busy-poll options are set or reported") {
  FASTMM_XDP_SKIP_UNLESS(veth_skip_reason());
  VethPair net;
  REQUIRE_MESSAGE(net.ok, "veth setup failed (ip netns / ip link)");
  REQUIRE(net.enter_rx());
  XdpConfig cfg;
  cfg.subscriptions.push_back({net.rx_if, ip("239.10.0.1"), kPortA, 0});
  cfg.frame_count = 128;
  cfg.mode = XdpMode::Generic;
  cfg.busy_poll = true;
  XdpDatagramSource src;
  const int rc = src.open(cfg);
  net.go_home();
  REQUIRE_MESSAGE(rc == 0, src.error());
  for (const auto& w : src.warnings()) MESSAGE(w);
  std::size_t n = 0;
  for (int i = 0; i < 10; ++i)
    n += src.poll([](std::span<const std::byte>, const RxMeta&) noexcept {});
  CHECK(n == 0);
}
