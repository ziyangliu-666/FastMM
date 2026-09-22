// DpdkDatagramSource over a veth pair in a user + network namespace: EAL without hugepages or
// PCI (--no-huge --no-pci --in-memory), the net_af_packet vdev on one end, a kernel UDP sender
// on the other. Built only with -DFASTMM_WITH_DPDK=ON; each case runs in its own process (the EAL
// initialises once per process).
#include "../net/netns_test_util.hpp"
#include "../net/user_tcp_test_util.hpp"

#include "fastmm/net/dpdk_datagram_source.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/udp_socket.hpp"

#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::net;
using namespace fastmm::net::test;

namespace {

bool sh(const std::string& cmd) {
  return std::system((cmd + " >/dev/null 2>&1").c_str()) == 0;
}

std::uint32_t ip(const char* text) {
  std::uint32_t v = 0;
  REQUIRE(parse_ipv4(text, v));
  return v;
}

bool make_veth() {
  return sh("ip link add dpdk0 type veth peer name dpdk1") &&
         sh("ip addr add 10.78.0.2/24 dev dpdk0") && sh("ip link set dpdk0 up multicast on") &&
         sh("ip addr add 10.78.0.1/24 dev dpdk1") && sh("ip link set dpdk1 up multicast on") &&
         sh("ip route replace 224.0.0.0/4 dev dpdk1");
}

int sender() {
  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  REQUIRE(fd >= 0);
  in_addr ifa{};
  ifa.s_addr = ip("10.78.0.1");
  REQUIRE(::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof ifa) == 0);
  return fd;
}

void send_to(int fd, const char* group, std::uint16_t port, const std::string& payload) {
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  to.sin_addr.s_addr = ip(group);
  REQUIRE(::sendto(
              fd, payload.data(), payload.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof to) ==
          static_cast<ssize_t>(payload.size()));
}

// dpdk0 without an address (the DPDK side), dpdk1 = 10.78.0.1 with its checksums computed (the
// af_packet PMD does not report a checksum left to offload) and no TSO/GSO.
bool make_bare_veth() {
  return sh("ip link add dpdk0 type veth peer name dpdk1") && sh("ip link set dpdk0 up") &&
         sh("ip addr add 10.78.0.1/24 dev dpdk1") && sh("ip link set dpdk1 up") &&
         sh("ethtool -K dpdk1 tso off gso off tx off");
}

DpdkConfig config() {
  DpdkConfig c;
  c.eal_args = {"--no-huge",
                "--no-pci",
                "--in-memory",
                "--no-telemetry",
                "-l",
                "0",
                "-m",
                "128",
                "--log-level=lib.eal:error",
                "--vdev=net_af_packet0,iface=dpdk0"};
  c.port = "net_af_packet0";
  c.subscriptions.push_back({"dpdk0", ip("239.10.0.1"), 30001, 0});
  c.subscriptions.push_back({"dpdk0", ip("239.10.0.2"), 30002, 0});
  c.mbufs = 1023;
  return c;
}

}  // namespace

TEST_CASE("DpdkDatagramSource: af_packet vdev on a veth delivers both lines with meta") {
  if (!in_multicast_netns()) return;
  REQUIRE(make_veth());
  DpdkDatagramSource src;
  const int r = src.open(config());
  REQUIRE_MESSAGE(r == 0, src.error());
  CHECK(src.port_name() == "net_af_packet0");
  const int tx = sender();
  const int n = 200;
  for (int i = 0; i < n; ++i) {
    send_to(tx,
            i % 2 == 0 ? "239.10.0.1" : "239.10.0.2",
            i % 2 == 0 ? 30001 : 30002,
            "msg-" + std::to_string(i));
  }
  send_to(tx, "239.10.0.1", 30009, "other port");  // no subscription
  std::vector<std::string> got;
  std::vector<std::uint8_t> lines;
  const std::int64_t deadline = Reactor::now_ns() + 5'000'000'000;
  while (Reactor::now_ns() < deadline && got.size() < static_cast<std::size_t>(n)) {
    src.poll([&](std::span<const std::byte> p, const RxMeta& m) noexcept {
      got.emplace_back(reinterpret_cast<const char*>(p.data()), p.size());
      lines.push_back(m.line);
      CHECK(m.src_ip == ip("10.78.0.1"));
      CHECK(m.t0_cycles.v != 0);
      CHECK(m.dst_port == (m.line == 0 ? 30001 : 30002));
    });
  }
  REQUIRE(got.size() == static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    CHECK(got[static_cast<std::size_t>(i)] == "msg-" + std::to_string(i));
    CHECK(lines[static_cast<std::size_t>(i)] == static_cast<std::uint8_t>(i % 2));
  }
  // The unmatched datagram and whatever else crossed the veth (IGMP, ARP) are counted, not
  // delivered.
  const std::int64_t settle = Reactor::now_ns() + 200'000'000;
  while (Reactor::now_ns() < settle && src.stats().unmatched == 0) {
    src.poll([](std::span<const std::byte>, const RxMeta&) noexcept {});
  }
  CHECK(src.stats().unmatched >= 1);
  CHECK(src.stats().datagrams == static_cast<std::uint64_t>(n));
  REQUIRE(src.refresh_stats() == 0);
  CHECK(src.stats().ipackets >= static_cast<std::uint64_t>(n));
  ::close(tx);
}

TEST_CASE("DpdkDatagramSource: a missing port and a second EAL configuration fail cleanly") {
  if (!in_multicast_netns()) return;
  REQUIRE(make_veth());
  DpdkConfig c = config();
  c.port = "net_af_packet9";
  DpdkDatagramSource src;
  CHECK(src.open(c) == -ENODEV);
  CHECK(src.error().find("net_af_packet9") != std::string::npos);
  c = config();
  c.eal_args.emplace_back("--no-shconf");
  CHECK(src.open(c) == -EALREADY);
  CHECK(src.open(config()) == 0);
}

TEST_CASE("DpdkDatagramSource: unicast lines with ARP for their address answered from the port") {
  if (!in_multicast_netns()) return;
  if (!sh("ethtool --version")) {
    MESSAGE("skipped: needs ethtool");
    return;
  }
  REQUIRE(make_bare_veth());
  DpdkConfig c = config();
  c.subscriptions.clear();
  c.subscriptions.push_back({"", ip("10.78.0.9"), 30001, 0});
  c.subscriptions.push_back({"", ip("10.78.0.9"), 30002, 0});
  DpdkDatagramSource src;
  REQUIRE_MESSAGE(src.open(c) == 0, src.error());
  const int tx = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  REQUIRE(tx >= 0);
  const int n = 100;
  std::vector<std::string> got;
  int sent = 0;
  const std::int64_t deadline = Reactor::now_ns() + 5'000'000'000;
  while (Reactor::now_ns() < deadline && got.size() < static_cast<std::size_t>(n)) {
    // The first datagrams wait in the kernel for the ARP answer only the port can give.
    if (sent < n) {
      send_to(tx, "10.78.0.9", sent % 2 == 0 ? 30001 : 30002, "u-" + std::to_string(sent));
      ++sent;
    }
    src.poll([&](std::span<const std::byte> p, const RxMeta& m) noexcept {
      got.emplace_back(reinterpret_cast<const char*>(p.data()), p.size());
      CHECK(m.dst_ip == ip("10.78.0.9"));
      CHECK(m.src_ip == ip("10.78.0.1"));
    });
  }
  CHECK(src.stats().arp_replies >= 1);
  REQUIRE(got.size() == static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) CHECK(got[static_cast<std::size_t>(i)] == "u-" + std::to_string(i));
  ::close(tx);
}

TEST_CASE("DpdkDatagramSource: UserTcp on the port echoes through the kernel TCP stack") {
  if (!in_multicast_netns()) return;
  if (!sh("ethtool --version")) {
    MESSAGE("skipped: needs ethtool");
    return;
  }
  REQUIRE(make_bare_veth());
  DpdkConfig c = config();
  c.subscriptions.clear();
  c.subscriptions.push_back({"", ip("10.78.0.2"), 30001, 0});
  DpdkDatagramSource src;
  REQUIRE_MESSAGE(src.open(c) == 0, src.error());

  const int lfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  REQUIRE(lfd >= 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(7000);
  a.sin_addr.s_addr = ip("10.78.0.1");
  REQUIRE(::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
  REQUIRE(::listen(lfd, 4) == 0);

  RecordingHandler h;
  UserTcpConfig tc;
  tc.local_mac = src.mac();
  tc.local_ip = ip("10.78.0.3");
  tc.remote_ip = ip("10.78.0.1");
  tc.remote_port = 7000;
  tc.rto_min_ns = 2'000'000;
  tc.rto_initial_ns = 20'000'000;
  UserTcp tcp(src.frame_tx(), h, tc);
  FrameSink sink;
  sink.fn = [](void* p, std::span<const std::byte> f) noexcept {
    auto* t = static_cast<UserTcp*>(p);
    t->on_frame(f, Reactor::now_ns());
    t->flush();
  };
  sink.ctx = &tcp;
  sink.ip = tc.local_ip;
  src.set_frame_sink(sink);
  REQUIRE(tcp.connect(Reactor::now_ns()));

  int cfd = -1;
  std::string sent;
  std::string pending;
  std::mt19937_64 gen(5);
  const std::size_t total = 256U << 10;
  const std::int64_t deadline = Reactor::now_ns() + 30'000'000'000;
  while (Reactor::now_ns() < deadline) {
    src.poll([](std::span<const std::byte>, const RxMeta&) noexcept {});
    if (Reactor::now_ns() >= tcp.next_timer_ns()) tcp.on_timer(Reactor::now_ns());
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
      REQUIRE(tcp.send(bytes_of(chunk)));
      sent += chunk;
    }
    if (sent.size() == total && h.data.size() == total) break;
    REQUIRE(h.closed == -1);
  }
  INFO("sent " << sent.size() << " echoed " << h.data.size() << " state " << to_string(tcp.state())
               << " retransmits " << tcp.stats().retransmits);
  REQUIRE(h.data.size() == total);
  CHECK(h.data == sent);
  CHECK(tcp.stats().bad_frames == 0);
  CHECK(src.stats().to_sink > 0);
  CHECK(src.stats().tx_frames > 0);
  CHECK(src.stats().tx_drops == 0);
  tcp.abort();
  if (cfd >= 0) ::close(cfd);
  ::close(lfd);
}
