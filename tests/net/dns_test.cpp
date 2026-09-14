#include "fastmm/net/dns.hpp"

#include "net_test_util.hpp"

#include <algorithm>

using namespace fastmm::net;
using namespace fastmm::net::test;

TEST_CASE("dns: resolve_sync") {
  SUBCASE("numeric host bypasses getaddrinfo") {
    auto r = resolve_sync("127.0.0.1", 1234);
    REQUIRE(r.ok());
    CHECK(r.addrs.size() == 1);
    CHECK(r.addrs[0].to_string() == "127.0.0.1:1234");
    CHECK(r.error_text().empty());
  }
  SUBCASE("localhost resolves to loopback") {
    auto r = resolve_sync("localhost", 80);
    REQUIRE(r.ok());
    const bool has_loopback = std::any_of(r.addrs.begin(), r.addrs.end(), [](const SockAddr& a) {
      return a.to_string() == "127.0.0.1:80" || a.to_string() == "[::1]:80";
    });
    CHECK(has_loopback);
    // IPv4 addresses are ordered first.
    bool seen_v6 = false;
    for (const auto& a : r.addrs) {
      if (a.family() == AF_INET6) seen_v6 = true;
      if (seen_v6) CHECK(a.family() == AF_INET6);
    }
  }
  SUBCASE("invalid host fails with a gai error") {
    auto r = resolve_sync("invalid.host.name.fastmm.test.", 1);
    CHECK_FALSE(r.ok());
    CHECK(r.gai_error != 0);
    CHECK_FALSE(r.error_text().empty());
  }
}

FASTMM_BACKEND_TEST("dns: AsyncResolver delivers on the reactor thread", test_dns_1) {
  Reactor reactor(backend);
  AsyncResolver resolver(reactor);
  const auto main_thread = std::this_thread::get_id();
  int done = 0;
  ResolveResult got;
  resolver.resolve("localhost", 443, [&](ResolveResult r) {
    CHECK(std::this_thread::get_id() == main_thread);
    got = std::move(r);
    ++done;
  });
  resolver.resolve("127.0.0.1", 7, [&](ResolveResult r) {
    CHECK(r.ok());
    CHECK(r.addrs[0].port() == 7);
    ++done;
  });
  REQUIRE(run_until(reactor, [&] { return done == 2; }, 5000));
  CHECK(got.ok());
  CHECK(got.addrs[0].port() == 443);
}

FASTMM_BACKEND_TEST("dns: AsyncResolver destructor with a queued request does not hang",
                    test_dns_2) {
  Reactor reactor(backend);
  {
    AsyncResolver resolver(reactor);
    resolver.resolve("localhost", 1, [](ResolveResult) {});
  }
  // Whatever was posted before the stop is still safe to run.
  reactor.run_once(0);
}
