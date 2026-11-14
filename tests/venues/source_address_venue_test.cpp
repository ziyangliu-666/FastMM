// [venues.<name>] source_ip / source_interface in the connectors: the REST channel and the
// blocking requests leave from the source address, the registry refuses a connector that cannot
// bind, and Binance accounts count request weight per source address. Endpoint selection
// ([venues.<name>] endpoint_select) goes the same way: the blocking requests connect to the
// fastest address, and a connector that cannot select refuses it.
#include "test_support.hpp"

#include "fastmm/net/endpoint_selector.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/tcp_socket.hpp"
#include "fastmm/venues/binance/binance_venue.hpp"
#include "fastmm/venues/blocking_http.hpp"
#include "fastmm/venues/registry.hpp"
#include "fastmm/venues/rest_channel.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

using namespace fastmm;
using namespace fastmm::venues;

namespace {

// The ip of the next connection `listener` accepts (3 s at most), "none" without one.
std::string accept_peer(net::TcpSocket& listener, net::Reactor* reactor = nullptr) {
  net::SockAddr peer;
  for (int i = 0; i < 3000; ++i) {
    net::TcpSocket s = listener.accept(&peer);
    if (s.valid()) {
      const std::string text = peer.to_string();
      return text.substr(0, text.rfind(':'));
    }
    if (reactor != nullptr) {
      reactor->run_once(1);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  return "none";
}

}  // namespace

TEST_CASE("venues.source_address: REST and blocking requests leave from the source address") {
  net::TcpSocket listener = net::TcpSocket::listen(net::SockAddr::loopback_v4(0));
  REQUIRE(listener.valid());
  const std::string url = "http://127.0.0.1:" + std::to_string(listener.local_addr()->port());

  net::Reactor reactor;
  RestChannelConfig rc;
  rc.base_url = url;
  rc.source = {"127.0.0.2", ""};
  RestChannel rest(reactor, rc);
  CHECK(rest.request("GET", "/api/v3/time", {}, {}, [](const net::HttpResponse&) {}));
  CHECK(accept_peer(listener, &reactor) == "127.0.0.2");

  BlockingHttpOptions bo;
  bo.timeout_ms = 300;
  bo.connect_timeout_ms = 300;
  bo.source = {"", "lo"};  // lo's address: 127.0.0.1
  std::thread t([&] {
    BlockingHttp http(url, bo);
    static_cast<void>(http.get("/api/v3/time"));
  });
  CHECK(accept_peer(listener) == "127.0.0.1");
  t.join();

  bo.source = {"127.0.0.4", ""};
  std::thread t2([&] {
    BlockingHttp http(url, bo);
    static_cast<void>(http.get("/api/v3/time"));
  });
  CHECK(accept_peer(listener) == "127.0.0.4");
  t2.join();

  // A family the source has no address of: nothing to connect to.
  bo.source = {"::1", ""};
  BlockingHttp v6_only(url, bo);
  const HttpReply r = v6_only.get("/api/v3/time");
  CHECK(r.error.find("family of the source address") != std::string::npos);
}

TEST_CASE("venues.source_address: Binance accounts share request weight per source address") {
  // A 429 seen from one address pauses the pool's accounts sending from it, not those sending
  // from another; the accounts without a source share the host's default address.
  const std::string url = "https://api.source-test.invalid";
  const auto make = [&](const char* name, const char* ip) {
    binance::BinanceVenueConfig c;
    c.name = name;
    c.rest_url = url;
    c.share_ip_weight = true;
    c.source.ip = ip;
    return std::make_unique<binance::BinanceVenue>(VenueId{0}, c);
  };
  const auto a = make("a", "127.0.0.2");
  const auto b = make("b", "127.0.0.2");
  const auto c = make("c", "127.0.0.3");
  const auto d = make("d", "");
  const auto e = make("e", "");
  const std::int64_t now = net::Reactor::now_ns();
  {
    const auto shared = shared_rate(ip_weight_key("binance_spot", url, "127.0.0.2"));
    const std::lock_guard lock(shared->m);
    shared->r.cooldown(60'000'000'000, now);
  }
  CHECK(a->rate_limiter().in_cooldown(now));
  CHECK(b->rate_limiter().in_cooldown(now));
  CHECK_FALSE(c->rate_limiter().in_cooldown(now));
  CHECK_FALSE(d->rate_limiter().in_cooldown(now));
  {
    const auto shared = shared_rate(ip_weight_key("binance_spot", url, ""));
    const std::lock_guard lock(shared->m);
    shared->r.cooldown(60'000'000'000, now);
  }
  CHECK(d->rate_limiter().in_cooldown(now));
  CHECK(e->rate_limiter().in_cooldown(now));
  CHECK_FALSE(c->rate_limiter().in_cooldown(now));
  CHECK(ip_weight_key("binance_spot", url, "") == "binance_spot " + url);
}

TEST_CASE("venues.source_address: a connector that cannot bind refuses the setting") {
  register_builtin_venues();
  VenueSection s;
  s.name = "feed";
  s.kind = "nasdaq_itch";
  s.source_ip = "127.0.0.2";
  Config cfg;
  cfg.venues.push_back(s);
  CHECK_THROWS_WITH_AS(validate_venues(cfg),
                       doctest::Contains("cannot bind its connections to a source address"),
                       ConfigError);
  const VenueEntry* binance = VenueRegistry::instance().find("binance_spot");
  REQUIRE(binance != nullptr);
  CHECK(binance->caps.bind_source);
}

TEST_CASE("venues.endpoint_select: blocking requests go to the fastest address") {
  net::TcpSocket slow = net::TcpSocket::listen(net::SockAddr::loopback_v4(0));
  net::TcpSocket fast = net::TcpSocket::listen(net::SockAddr::loopback_v4(0));
  REQUIRE((slow.valid() && fast.valid()));
  const std::uint16_t slow_port = slow.local_addr()->port();
  const std::uint16_t fast_port = fast.local_addr()->port();
  net::EndpointHooks hooks;
  hooks.resolve = [&](std::string_view, std::uint16_t) {
    net::ResolveResult r;
    r.addrs = {net::SockAddr::loopback_v4(slow_port), net::SockAddr::loopback_v4(fast_port)};
    return r;
  };
  hooks.connect_ns = [&](const net::SockAddr& peer, const net::SourceAddress&, std::uint32_t) {
    return std::optional<std::int64_t>(peer.port() == fast_port ? 100'000 : 300'000);
  };
  net::EndpointSelectConfig sc;
  sc.probe_interval_s = 0;
  net::EndpointSelector::instance().configure(sc, hooks);

  BlockingHttpOptions bo;
  bo.timeout_ms = 300;
  bo.connect_timeout_ms = 300;
  bo.select_endpoint = true;
  std::thread t([&] {
    BlockingHttp http("http://venue.test:" + std::to_string(slow_port), bo);
    static_cast<void>(http.get("/api/v3/time"));
  });
  CHECK(accept_peer(fast) == "127.0.0.1");
  t.join();
  net::EndpointSelector::instance().configure({});
}

TEST_CASE("venues.endpoint_select: a connector that cannot select refuses the setting") {
  register_builtin_venues();
  VenueSection s;
  s.name = "feed";
  s.kind = "nasdaq_itch";
  Config cfg;
  s.endpoint_select = false;  // saying no is always fine
  cfg.venues.push_back(s);
  validate_venues(cfg);
  cfg.venues[0].endpoint_select = true;
  CHECK_THROWS_WITH_AS(
      validate_venues(cfg), doctest::Contains("does not select its endpoints"), ConfigError);
}
