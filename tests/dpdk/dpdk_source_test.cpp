// DpdkDatagramSource over a veth pair in a user + network namespace: EAL without hugepages or
// PCI (--no-huge --no-pci --in-memory), the net_af_packet vdev on one end, a kernel UDP sender
// on the other. Built only with -DFASTMM_WITH_DPDK=ON; each case runs in its own process (the EAL
// initialises once per process).
#include "../net/netns_test_util.hpp"

#include "fastmm/net/dpdk_datagram_source.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/udp_socket.hpp"

#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
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
