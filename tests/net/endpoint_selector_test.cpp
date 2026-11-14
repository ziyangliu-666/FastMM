// Endpoint selection: probes rank a host's addresses, new connections take the fastest and spread
// over the equally fast, a failed address is passed over, addresses gone from the lookups are
// forgotten, and a WebSocket connection fails over and moves to a faster address.
#include "fastmm/net/endpoint_selector.hpp"

#include "net_test_util.hpp"

#include "fastmm/net/connection.hpp"
#include "fastmm/net/http_server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace fastmm::net;
using namespace fastmm::net::test;

namespace {

SockAddr at(std::uint16_t port) {
  return SockAddr::loopback_v4(port);
}

// A host whose lookups answer `answer` and whose connects take `rtt_us` per port (absent: the
// connect fails). Thread-safe: lookups run on resolver helpers.
struct FakeNet {
  std::mutex mutex;
  std::vector<SockAddr> answer;
  std::map<std::uint16_t, std::int64_t> rtt_us;
  std::atomic<int> lookups{0};
  std::atomic<int> connects{0};

  void set(std::uint16_t port, std::int64_t us) {
    const std::lock_guard lock(mutex);
    rtt_us[port] = us;
  }
  void answer_with(std::vector<SockAddr> a) {
    const std::lock_guard lock(mutex);
    answer = std::move(a);
  }
  EndpointHooks hooks() {
    EndpointHooks h;
    h.resolve = [this](std::string_view, std::uint16_t) {
      ++lookups;
      const std::lock_guard lock(mutex);
      ResolveResult r;
      r.addrs = answer;
      return r;
    };
    h.connect_ns = [this](const SockAddr& peer,
                          const SourceAddress&,
                          std::uint32_t) -> std::optional<std::int64_t> {
      ++connects;
      const std::lock_guard lock(mutex);
      const auto it = rtt_us.find(peer.port());
      if (it == rtt_us.end()) return std::nullopt;
      return it->second * 1000;
    };
    return h;
  }
};

EndpointSelectConfig test_config() {
  EndpointSelectConfig c;
  c.enabled = true;
  c.samples = 3;
  c.probe_interval_s = 0;  // no background thread; tests call probe_all()
  c.spread_us = 20;
  c.switch_us = 50;
  c.move_check_ms = 10;
  return c;
}

// Restores the process's endpoint selection when a test ends.
struct SelectorScope {
  SelectorScope(const EndpointSelectConfig& c, EndpointHooks h) {
    EndpointSelector::instance().configure(c, std::move(h));
  }
  ~SelectorScope() { EndpointSelector::instance().configure({}); }
  SelectorScope(const SelectorScope&) = delete;
  SelectorScope& operator=(const SelectorScope&) = delete;
};

}  // namespace

TEST_CASE("net.endpoint: a probe times a connect, and fails on a closed port") {
  std::uint16_t port = 0;
  TcpSocket listener = listen_ephemeral(port);
  const auto t = probe_connect_ns(at(port), {}, 1000);
  REQUIRE(t);
  CHECK(*t > 0);
  CHECK(*t < 1'000'000'000);
  std::uint16_t closed = 0;
  { TcpSocket gone = listen_ephemeral(closed); }
  CHECK_FALSE(probe_connect_ns(at(closed), {}, 1000));
  // From a source address: 127.0.0.2 is on lo.
  CHECK(probe_connect_ns(at(port), {"127.0.0.2", ""}, 1000));
}

TEST_CASE("net.endpoint: rank by connect time, spread leases, pass over failures") {
  FakeNet net;
  net.answer_with({at(1), at(2), at(3), at(4)});
  net.set(1, 300);
  net.set(2, 110);
  net.set(3, 100);  // 4 never connects
  EndpointHost host("venue.test", 443, {}, test_config(), net.hooks());
  CHECK_FALSE(host.probed());
  CHECK_FALSE(host.acquire(false));

  const ResolveResult r = host.refresh();
  REQUIRE(r.ok());
  REQUIRE(r.addrs.size() == 4);
  CHECK(r.addrs[0].port() == 3);
  CHECK(r.addrs[1].port() == 2);
  CHECK(r.addrs[2].port() == 1);
  CHECK(r.addrs[3].port() == 4);  // unreachable: last
  CHECK(host.probed());
  // A dead address costs one connect, not one per sample.
  CHECK(net.connects.load() == 3 * 3 + 1);
  const std::vector<EndpointAddress> a = host.addresses();
  CHECK(a[0].rtt_ns == 100'000);
  CHECK(a[0].samples == 3);
  CHECK(a[3].rtt_ns == -1);

  // 3 and 2 are within 20 us: leases alternate between them; 1 is 200 us slower and never taken.
  const auto c1 = host.acquire(true);
  const auto c2 = host.acquire(true);
  const auto c3 = host.acquire(true);
  REQUIRE((c1 && c2 && c3));
  CHECK(c1->addr.port() == 3);
  CHECK(c1->rtt_ns == 100'000);
  CHECK(c1->best_ns == 100'000);
  CHECK(c1->addresses == 4);
  CHECK(c2->addr.port() == 2);
  CHECK(c3->addr.port() == 3);
  CHECK(c3->users == 2);
  // Without a lease: the fastest, whoever is on it.
  CHECK(host.acquire(false)->addr.port() == 3);
  host.release(at(3));
  host.release(at(3));
  host.release(at(2));
  for (const EndpointAddress& e : host.addresses()) CHECK(e.users == 0);

  // A failure moves new connections to the next address, and ranks it last.
  host.failed(at(3));
  CHECK(host.acquire(false)->addr.port() == 2);
  CHECK(host.ranked().back().port() != 3);
  host.failed(at(2));
  CHECK(host.acquire(false)->addr.port() == 1);
  host.failed(at(1));
  // All of them failed: the one that failed longest ago is tried rather than none.
  CHECK(host.acquire(false)->addr.port() == 4);

  // Every measured address has failed: nowhere better to move to.
  CHECK_FALSE(host.should_move(at(1)));
}

TEST_CASE("net.endpoint: who should move") {
  FakeNet net;
  net.answer_with({at(1), at(2), at(3)});
  net.set(1, 100);
  net.set(2, 115);
  net.set(3, 200);
  EndpointSelectConfig cfg = test_config();
  cfg.forget_ms = 50;
  EndpointHost host("venue.test", 443, {}, cfg, net.hooks());
  static_cast<void>(host.refresh());
  const std::uint64_t g = host.generation();
  CHECK(g == 1);
  CHECK_FALSE(host.should_move(at(1)));  // the fastest
  CHECK_FALSE(host.should_move(at(2)));  // 15 us behind: within switch_us
  CHECK(host.should_move(at(3)));        // 100 us behind

  // 2 holds a connection; then the lookups stop returning 2 and 3.
  REQUIRE(host.acquire(true));  // 1
  REQUIRE(host.acquire(true)->addr.port() == 2);
  net.answer_with({at(1)});
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  static_cast<void>(host.refresh());
  CHECK(host.generation() == g + 1);
  // 3 had no connection and is forgotten; 2 is kept while used, but its connection should move.
  const std::vector<EndpointAddress> a = host.addresses();
  CHECK(a.size() == 2);
  CHECK(host.should_move(at(2)));
  CHECK(host.ranked().front().port() == 1);
  host.release(at(2));
  static_cast<void>(host.refresh());
  CHECK(host.addresses().size() == 1);

  // Rotating answers add up.
  net.answer_with({at(4)});
  net.set(4, 90);
  static_cast<void>(host.refresh());
  CHECK(host.addresses().size() == 2);
  CHECK(host.ranked().front().port() == 4);
  CHECK(host.should_move(at(1)) == false);  // 10 us behind
}

TEST_CASE("net.endpoint: lookup failure keeps the known addresses") {
  FakeNet net;
  net.set(1, 100);
  EndpointHost host("venue.test", 443, {}, test_config(), net.hooks());
  ResolveResult none = host.refresh();
  CHECK_FALSE(none.ok());
  CHECK(none.gai_error != 0);
  net.answer_with({at(1)});
  REQUIRE(host.refresh().ok());
  net.answer_with({});
  const ResolveResult kept = host.refresh();
  REQUIRE(kept.ok());
  CHECK(kept.addrs.front().port() == 1);
}

TEST_CASE("net.endpoint: the selector shares a host per name, port and source") {
  FakeNet net;
  SelectorScope scope(test_config(), net.hooks());
  EndpointSelector& sel = EndpointSelector::instance();
  CHECK(sel.selects(std::nullopt));
  CHECK_FALSE(sel.selects(false));
  const auto a = sel.host("venue.test", 443, {});
  const auto b = sel.host("venue.test", 443, {});
  const auto c = sel.host("venue.test", 9443, {});
  const auto d = sel.host("venue.test", 443, {"127.0.0.2", ""});
  CHECK(a == b);
  CHECK(a != c);
  CHECK(a != d);
  CHECK(sel.host("127.0.0.1", 443, {}) == nullptr);  // a numeric address is used as it is
  net.answer_with({at(1)});
  net.set(1, 100);
  sel.probe_all();
  CHECK(a->generation() == 1);
  CHECK(c->generation() == 1);
  EndpointSelector::instance().configure({});
  CHECK_FALSE(sel.selects(std::nullopt));
  CHECK(sel.selects(true));
}

namespace {

struct Events {
  int live = 0;
  int subscriptions = 0;
  void on_state(ConnState s) { live += s == ConnState::Live ? 1 : 0; }
  void on_text(std::string_view, std::int64_t) {}
  void on_binary(std::span<const std::byte>, std::int64_t) {}
  void on_connected_send_subscriptions() { ++subscriptions; }
};

class Venue final : public WsSessionHandler {
 public:
  void on_open(WsSession& s) override {
    sessions.push_back(&s);
    ++opened;
  }
  void on_text(WsSession&, std::string_view) override {}
  void on_close(WsSession& s, std::uint16_t, std::string_view) override { forget(s); }
  void on_error(WsSession& s, NetError, std::string_view) override { forget(s); }
  void forget(WsSession& s) { std::erase(sessions, &s); }
  std::vector<WsSession*> sessions;
  int opened = 0;
};

struct Server {
  explicit Server(Reactor& r) : http(r, [](TcpSocket&& s) { return PlainStream(std::move(s)); }) {
    http.set_ws_handler(&venue);
    REQUIRE(http.listen(SockAddr::loopback_v4(0)));
  }
  Venue venue;
  HttpServer<PlainStream> http;
};

ConnectionConfig config_for(std::uint16_t port) {
  ConnectionConfig cfg;
  cfg.url = "ws://venue.test:" + std::to_string(port) + "/ws";
  cfg.select_endpoint = true;
  cfg.backoff = BackoffConfig{.base_ms = 20, .max_ms = 100, .jitter = 0.0};
  cfg.stale_ms = 10'000;
  cfg.dead_ms = 20'000;
  cfg.connect_timeout_ms = 2000;
  return cfg;
}

}  // namespace

FASTMM_BACKEND_TEST("net.endpoint: a connection takes the fastest address and fails over",
                    test_endpoint_failover) {
  Reactor reactor(backend);
  Server slow(reactor);
  auto fast = std::make_unique<Server>(reactor);
  FakeNet net;
  net.answer_with({at(slow.http.port()), at(fast->http.port())});
  net.set(slow.http.port(), 300);
  net.set(fast->http.port(), 100);
  SelectorScope scope(test_config(), net.hooks());

  Events ev;
  Connection<PlainStream, Events> conn(reactor, config_for(slow.http.port()), ev);
  REQUIRE(conn.endpoints() != nullptr);
  conn.connect();
  REQUIRE(run_until(reactor, [&] { return conn.state() == ConnState::Live; }));
  REQUIRE(run_until(reactor, [&] { return fast->venue.opened == 1; }));
  CHECK(slow.venue.opened == 0);
  CHECK(conn.peer()->port() == fast->http.port());
  CHECK(conn.endpoints()->addresses().front().users == 1);

  // The fast address goes away; the probes still rank it first, but the connect fails there and
  // the next attempt takes the other address.
  const std::uint16_t fast_port = fast->http.port();
  fast.reset();
  REQUIRE(run_until(reactor, [&] { return slow.venue.opened == 1; }, 5000));
  REQUIRE(run_until(reactor, [&] { return conn.state() == ConnState::Live; }));
  CHECK(conn.peer()->port() == slow.http.port());
  CHECK(conn.stats().reconnects >= 1);
  for (const EndpointAddress& a : conn.endpoints()->addresses())
    CHECK(a.users == (a.addr.port() == fast_port ? 0U : 1U));
  conn.close();
  for (const EndpointAddress& a : conn.endpoints()->addresses()) CHECK(a.users == 0);
}

FASTMM_BACKEND_TEST("net.endpoint: copies spread over the fast addresses", test_endpoint_spread) {
  Reactor reactor(backend);
  Server a(reactor);
  Server b(reactor);
  FakeNet net;
  net.answer_with({at(a.http.port()), at(b.http.port())});
  net.set(a.http.port(), 100);
  net.set(b.http.port(), 105);
  SelectorScope scope(test_config(), net.hooks());

  Events e1;
  Events e2;
  Connection<PlainStream, Events> c1(reactor, config_for(a.http.port()), e1);
  Connection<PlainStream, Events> c2(reactor, config_for(a.http.port()), e2);
  CHECK(c1.endpoints() == c2.endpoints());
  c1.connect();
  c2.connect();
  REQUIRE(run_until(reactor, [&] { return c1.is_live() && c2.is_live(); }));
  CHECK(a.venue.opened == 1);
  CHECK(b.venue.opened == 1);
  CHECK(c1.peer()->port() != c2.peer()->port());
  c1.close();
  c2.close();
}

FASTMM_BACKEND_TEST("net.endpoint: a connection that rolls over moves to a faster address",
                    test_endpoint_move) {
  Reactor reactor(backend);
  Server a(reactor);
  Server b(reactor);
  FakeNet net;
  net.answer_with({at(a.http.port()), at(b.http.port())});
  net.set(a.http.port(), 100);
  net.set(b.http.port(), 300);
  SelectorScope scope(test_config(), net.hooks());

  Events ev;
  ConnectionConfig cfg = config_for(a.http.port());
  cfg.max_lifetime_ms = 3'600'000;  // can roll over
  Connection<PlainStream, Events> conn(reactor, cfg, ev);
  conn.connect();
  REQUIRE(run_until(reactor, [&] { return conn.state() == ConnState::Live; }));
  CHECK(conn.peer()->port() == a.http.port());

  // A probe within switch_us changes nothing.
  net.set(b.http.port(), 80);
  EndpointSelector::instance().probe_all();
  for (int i = 0; i < 10; ++i) reactor.run_once(10);
  CHECK(conn.peer()->port() == a.http.port());
  CHECK(conn.stats().endpoint_moves == 0);

  // b is now 200 us faster: the connection rolls over to it without leaving Live.
  net.set(a.http.port(), 300);
  net.set(b.http.port(), 100);
  EndpointSelector::instance().probe_all();
  REQUIRE(run_until(reactor, [&] { return conn.peer()->port() == b.http.port(); }));
  REQUIRE(run_until(reactor, [&] { return a.venue.sessions.empty(); }));
  CHECK(conn.stats().endpoint_moves == 1);
  CHECK(conn.stats().rollovers == 1);
  CHECK(conn.stats().reconnects == 0);
  CHECK(ev.live == 1);
  CHECK(ev.subscriptions == 2);
  CHECK(b.venue.opened == 1);

  // Without a lifetime the connection stays where it is until it reconnects.
  Events ev2;
  Connection<PlainStream, Events> fixed(reactor, config_for(a.http.port()), ev2);
  fixed.connect();
  REQUIRE(run_until(reactor, [&] { return fixed.state() == ConnState::Live; }));
  const std::uint16_t first = fixed.peer()->port();
  net.set(a.http.port(), first == a.http.port() ? 400 : 100);
  net.set(b.http.port(), first == b.http.port() ? 400 : 100);
  EndpointSelector::instance().probe_all();
  for (int i = 0; i < 10; ++i) reactor.run_once(10);
  CHECK(fixed.peer()->port() == first);
  CHECK(fixed.stats().endpoint_moves == 0);
  fixed.close();
  conn.close();
}
