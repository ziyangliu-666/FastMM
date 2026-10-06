// SourceAddress: the setting checked against this host, and connections that leave from it. Linux
// delivers all of 127.0.0.0/8 to lo, so 127.0.0.2 is an address of every host to bind to.
#include "fastmm/net/source_address.hpp"

#include "net_test_util.hpp"

#include "fastmm/net/connection.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <string>
#include <vector>

using namespace fastmm::net;
using namespace fastmm::net::test;

namespace {

std::string ip_of(const SockAddr& a) {
  std::string s = a.to_string();
  return s.substr(0, s.rfind(':'));
}

// The peer of the next connection `listener` accepts, within `r`'s loop.
std::string accept_peer(Reactor& r, TcpSocket& listener) {
  SockAddr peer;
  TcpSocket accepted;
  static_cast<void>(run_until(r, [&] {
    accepted = listener.accept(&peer);
    return accepted.valid();
  }));
  return accepted.valid() ? ip_of(peer) : std::string("none");
}

struct Quiet {
  void on_state(ConnState) {}
  void on_text(std::string_view, std::int64_t) {}
  void on_binary(std::span<const std::byte>, std::int64_t) {}
  void on_connected_send_subscriptions() {}
};

}  // namespace

TEST_CASE("net.source_address: the setting, and whether this host has it") {
  CHECK(source_syntax_error({}).empty());
  CHECK(source_syntax_error({"127.0.0.2", ""}).empty());
  CHECK(source_syntax_error({"::1", ""}).empty());
  CHECK(source_syntax_error({"127.0.0.2", "lo"}).find("exclusive") != std::string::npos);
  CHECK(source_syntax_error({"host.example", ""}).find("not an IPv4 or IPv6") != std::string::npos);
  CHECK(source_syntax_error({"", "an-interface-name-too-long"}).find("longer") !=
        std::string::npos);

  CHECK(check_source({}).empty());
  CHECK(check_source({"127.0.0.1", ""}).empty());
  CHECK(check_source({"127.0.0.2", ""}).empty());  // inside lo's 127.0.0.0/8
  CHECK(check_source({"192.0.2.77", ""}).find("not an address of this host") != std::string::npos);
  CHECK(check_source({"", "lo"}).empty());
  CHECK(check_source({"", "nosuchif0"}).find("does not exist") != std::string::npos);

  CHECK(source_label({}).empty());
  CHECK(source_label({"127.0.0.2", ""}) == "127.0.0.2");
  CHECK(source_label({"0:0::1", ""}) == "::1");
  CHECK(source_label({"", "lo"}) == "127.0.0.1");
  CHECK(source_label({"", "nosuchif0"}) == "if:nosuchif0");
}

TEST_CASE("net.source_address: the bind address and the peers it can reach") {
  const SourceAddress v4{"127.0.0.2", ""};
  const auto a = source_bind_address(v4, AF_INET);
  REQUIRE(a);
  CHECK(ip_of(*a) == "127.0.0.2");
  CHECK(a->port() == 0);
  CHECK_FALSE(source_bind_address(v4, AF_INET6));
  CHECK_FALSE(source_bind_address({}, AF_INET));
  const auto lo = source_bind_address({"", "lo"}, AF_INET);
  REQUIRE(lo);
  CHECK(ip_of(*lo) == "127.0.0.1");

  std::vector<SockAddr> peers = {*SockAddr::from_ip("::1", 443), SockAddr::loopback_v4(443)};
  keep_reachable({}, peers);
  CHECK(peers.size() == 2);
  keep_reachable(v4, peers);
  REQUIRE(peers.size() == 1);
  CHECK(peers[0].family() == AF_INET);
  keep_reachable({"::1", ""}, peers);
  CHECK(peers.empty());
}

TEST_CASE("net.source_address: a socket connects from the address it was bound to") {
  Reactor reactor;
  TcpSocket listener = TcpSocket::listen(SockAddr::loopback_v4(0));
  REQUIRE(listener.valid());
  const std::uint16_t port = listener.local_addr()->port();
  const SockAddr to = SockAddr::loopback_v4(port);

  TcpSocket plain;
  REQUIRE(plain.connect(to) != ConnectStatus::Error);
  CHECK(accept_peer(reactor, listener) == "127.0.0.1");

  const SockAddr from = *SockAddr::from_ip("127.0.0.2", 0);
  TcpSocket bound;
  REQUIRE(bound.connect(to, &from) != ConnectStatus::Error);
  CHECK(accept_peer(reactor, listener) == "127.0.0.2");

  // An address the host does not have fails at the bind.
  const SockAddr foreign = *SockAddr::from_ip("192.0.2.77", 0);
  TcpSocket refused;
  CHECK(refused.connect(to, &foreign) == ConnectStatus::Error);
  CHECK(refused.last_error() == EADDRNOTAVAIL);
}

TEST_CASE("net.source_address: a WebSocket connection leaves from its source address") {
  Reactor reactor;
  TcpSocket listener = TcpSocket::listen(SockAddr::loopback_v4(0));
  REQUIRE(listener.valid());
  ConnectionConfig cfg;
  cfg.url = "ws://127.0.0.1:" + std::to_string(listener.local_addr()->port()) + "/ws";
  cfg.source = {"127.0.0.3", ""};
  Quiet q;
  Connection<PlainStream, Quiet> conn(reactor, cfg, q);
  conn.connect();
  CHECK(accept_peer(reactor, listener) == "127.0.0.3");
  conn.close();
}
