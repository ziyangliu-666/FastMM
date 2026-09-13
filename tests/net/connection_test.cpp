#include "fastmm/net/connection.hpp"

#include "net_test_util.hpp"

#include "fastmm/net/http_server.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

using namespace fastmm::net;
using namespace fastmm::net::test;

namespace {

struct Events {
  std::vector<ConnState> states;
  std::vector<std::string> texts;
  int subscriptions_sent = 0;
  std::function<void()> subscribe;  // set after the Connection exists

  void on_state(ConnState s) { states.push_back(s); }
  void on_text(std::string_view t, std::int64_t) { texts.emplace_back(t); }
  void on_binary(std::span<const std::byte>, std::int64_t) {}
  void on_connected_send_subscriptions() {
    ++subscriptions_sent;
    if (subscribe) subscribe();
  }
  int count(ConnState s) const {
    return static_cast<int>(std::count(states.begin(), states.end(), s));
  }
  ConnState last() const { return states.empty() ? ConnState::Idle : states.back(); }
};

// Server handler: acknowledges SUBSCRIBE, tracks concurrently open sessions.
class VenueHandler final : public WsSessionHandler {
 public:
  void on_open(WsSession& s) override {
    sessions.push_back(&s);
    ++opened;
    max_concurrent = std::max(max_concurrent, static_cast<int>(sessions.size()));
  }
  void on_text(WsSession& s, std::string_view t) override {
    received.emplace_back(t);
    if (t == "SUBSCRIBE" && ack) s.send_text("{\"result\":null,\"id\":1}");
  }
  void on_close(WsSession& s, std::uint16_t, std::string_view) override { forget(s); }
  void on_error(WsSession& s, NetError, std::string_view) override { forget(s); }
  void forget(WsSession& s) {
    sessions.erase(std::remove(sessions.begin(), sessions.end(), &s), sessions.end());
  }

  std::vector<WsSession*> sessions;
  std::vector<std::string> received;
  int opened = 0;
  int max_concurrent = 0;
  bool ack = true;
};

ConnectionConfig fast_config(std::string url) {
  ConnectionConfig cfg;
  cfg.url = std::move(url);
  cfg.backoff = BackoffConfig{.base_ms = 20, .max_ms = 100, .jitter = 0.0};
  cfg.stale_ms = 80;
  cfg.dead_ms = 200;
  cfg.connect_timeout_ms = 2000;
  cfg.ws.recv_capacity = 64 * 1024;
  cfg.ws.send_capacity = 64 * 1024;
  return cfg;
}

}  // namespace

TEST_CASE("connection: connect reaches Live through every state and subscribes") {
  Reactor reactor;
  VenueHandler venue;
  HttpServer<PlainStream> server(reactor, [](TcpSocket&& s) { return PlainStream(std::move(s)); });
  server.set_ws_handler(&venue);
  REQUIRE(server.listen(SockAddr::loopback_v4(0)));

  Events ev;
  Connection<PlainStream, Events> conn(
      reactor,
      fast_config("ws://127.0.0.1:" + std::to_string(server.port()) + "/ws?streams=a"),
      ev);
  ev.subscribe = [&] { conn.send_text("SUBSCRIBE"); };
  CHECK(conn.state() == ConnState::Idle);
  conn.connect();
  REQUIRE(run_until(reactor, [&] { return conn.state() == ConnState::Live && !ev.texts.empty(); }));
  CHECK(ev.states == std::vector<ConnState>{ConnState::Resolving,
                                            ConnState::Connecting,
                                            ConnState::WsHandshake,
                                            ConnState::Authenticating,
                                            ConnState::Subscribing,
                                            ConnState::Live});
  CHECK(ev.subscriptions_sent == 1);
  REQUIRE(run_until(reactor, [&] { return !venue.received.empty(); }));
  CHECK(venue.received[0] == "SUBSCRIBE");
  CHECK(ev.texts[0] == "{\"result\":null,\"id\":1}");
  CHECK(conn.stats().connects == 1);
  CHECK(conn.stats().frames_tx == 1);
  CHECK(conn.last_rx_ns() > 0);

  SUBCASE("explicit close goes through Closing to Idle and does not reconnect") {
    conn.close();
    CHECK(ev.last() == ConnState::Idle);
    CHECK(ev.count(ConnState::Closing) == 1);
    REQUIRE(run_until(reactor, [&] { return venue.sessions.empty(); }));
    for (int i = 0; i < 10; ++i) reactor.run_once(10);
    CHECK(conn.state() == ConnState::Idle);
    CHECK(ev.count(ConnState::Backoff) == 0);
    CHECK_FALSE(conn.send_text("x"));
  }
  SUBCASE("server drop -> Backoff -> reconnect -> Live again") {
    REQUIRE(venue.sessions.size() == 1);
    venue.sessions[0]->close_abrupt();
    REQUIRE(run_until(reactor, [&] { return ev.count(ConnState::Backoff) == 1; }));
    REQUIRE(run_until(reactor, [&] { return ev.count(ConnState::Live) == 2; }));
    CHECK(conn.stats().reconnects == 1);
    CHECK(conn.stats().connects == 2);
    CHECK(ev.subscriptions_sent == 2);
    CHECK(venue.opened == 2);
  }
  SUBCASE("manual auth/subscribe acknowledgement") {
    conn.close();
    Events ev2;
    ConnectionConfig cfg = fast_config("ws://127.0.0.1:" + std::to_string(server.port()) + "/ws");
    cfg.manual_auth = true;
    cfg.manual_subscribe = true;
    Connection<PlainStream, Events> manual(reactor, cfg, ev2);
    ev2.subscribe = [&] { manual.send_text("SUBSCRIBE"); };
    manual.connect();
    REQUIRE(run_until(reactor, [&] { return manual.state() == ConnState::Authenticating; }));
    for (int i = 0; i < 5; ++i) reactor.run_once(5);
    CHECK(manual.state() == ConnState::Authenticating);
    CHECK(ev2.subscriptions_sent == 0);
    manual.auth_done();
    CHECK(manual.state() == ConnState::Subscribing);
    CHECK(ev2.subscriptions_sent == 1);
    REQUIRE(
        run_until(reactor, [&] { return !ev2.texts.empty(); }));  // ack arrives while Subscribing
    CHECK(manual.state() == ConnState::Subscribing);
    manual.subscribe_done();
    CHECK(manual.state() == ConnState::Live);
  }
}

TEST_CASE("connection: stale detection with a silent server, then dead -> reconnect") {
  Reactor reactor;
  VenueHandler venue;
  venue.ack = false;  // never sends anything
  HttpServer<PlainStream> server(reactor, [](TcpSocket&& s) { return PlainStream(std::move(s)); });
  server.set_ws_handler(&venue);
  REQUIRE(server.listen(SockAddr::loopback_v4(0)));

  Events ev;
  Connection<PlainStream, Events> conn(
      reactor, fast_config("ws://127.0.0.1:" + std::to_string(server.port()) + "/"), ev);
  conn.connect();
  REQUIRE(run_until(reactor, [&] { return conn.state() == ConnState::Live; }));
  REQUIRE(run_until(reactor, [&] { return conn.state() == ConnState::Stale; }, 2000));
  CHECK(conn.stats().stale_events == 1);
  CHECK(conn.is_live());

  SUBCASE("traffic clears the stale flag") {
    venue.sessions[0]->send_text("alive");
    REQUIRE(run_until(reactor, [&] { return conn.state() == ConnState::Live; }));
  }
  SUBCASE("dead_ms forces a reconnect") {
    REQUIRE(run_until(reactor, [&] { return conn.stats().dead_events == 1; }, 3000));
    CHECK(ev.count(ConnState::Backoff) == 1);
    REQUIRE(run_until(reactor, [&] { return ev.count(ConnState::Live) == 2; }));
    CHECK(venue.opened == 2);
  }
  SUBCASE("client heartbeat pings keep last_rx fresh via pongs") {
    conn.close();
    Events ev2;
    ConnectionConfig cfg = fast_config("ws://127.0.0.1:" + std::to_string(server.port()) + "/");
    cfg.heartbeat_interval_ms = 20;
    Connection<PlainStream, Events> hb(reactor, cfg, ev2);
    hb.connect();
    REQUIRE(run_until(reactor, [&] { return hb.state() == ConnState::Live; }));
    const auto deadline = Reactor::now_ns() + 250'000'000;
    while (Reactor::now_ns() < deadline) reactor.run_once(10);
    CHECK(hb.state() == ConnState::Live);  // pongs count as traffic
    CHECK(hb.stats().stale_events == 0);
    CHECK(hb.stats().frames_tx >= 5);
  }
}

TEST_CASE("connection: max_lifetime rollover is make-before-break") {
  Reactor reactor;
  VenueHandler venue;
  HttpServer<PlainStream> server(reactor, [](TcpSocket&& s) { return PlainStream(std::move(s)); });
  server.set_ws_handler(&venue);
  REQUIRE(server.listen(SockAddr::loopback_v4(0)));

  Events ev;
  ConnectionConfig cfg = fast_config("ws://127.0.0.1:" + std::to_string(server.port()) + "/");
  cfg.max_lifetime_ms = 60;
  // This test is about rollover, not health checks. The server never sends traffic, so with the
  // 80 ms stale / 200 ms dead defaults a slow run (sanitizers) legitimately goes Stale -> Live or
  // declares the old session dead, which would add Live transitions unrelated to rollover.
  cfg.stale_ms = 10'000;
  cfg.dead_ms = 20'000;
  Connection<PlainStream, Events> conn(reactor, cfg, ev);
  ev.subscribe = [&] { conn.send_text("SUBSCRIBE"); };
  conn.connect();
  REQUIRE(run_until(reactor, [&] { return conn.state() == ConnState::Live; }));
  REQUIRE(run_until(
      reactor, [&] { return ev.subscriptions_sent >= 3 && venue.received.size() >= 3; }, 3000));
  CHECK(venue.opened >= 3);
  CHECK(venue.max_concurrent == 2);  // new session opened while the old one was still up
  CHECK(conn.stats().rollovers >= 2);
  CHECK(conn.stats().reconnects == 0);
  CHECK(ev.count(ConnState::Backoff) == 0);
  CHECK(conn.stats().stale_events == 0);
  CHECK(conn.stats().dead_events == 0);
  CHECK(ev.count(ConnState::Live) == 1);  // rollover never leaves Live
  CHECK(ev.subscriptions_sent >= 3);      // each new session re-subscribes
  // Every SUBSCRIBE went to a distinct session (the newest one).
  CHECK(venue.received.size() >= 3);
  REQUIRE(run_until(reactor, [&] { return venue.sessions.size() == 1; }));
  conn.close();
}

TEST_CASE("connection: refused connects back off until the server appears") {
  Reactor reactor;
  std::uint16_t port = 0;
  { TcpSocket probe = listen_ephemeral(port); }
  Events ev;
  Connection<PlainStream, Events> conn(
      reactor, fast_config("ws://127.0.0.1:" + std::to_string(port) + "/"), ev);
  conn.connect();
  REQUIRE(run_until(reactor, [&] { return ev.count(ConnState::Backoff) >= 2; }, 3000));
  CHECK(conn.stats().reconnects >= 2);

  VenueHandler venue;
  HttpServer<PlainStream> server(reactor, [](TcpSocket&& s) { return PlainStream(std::move(s)); });
  server.set_ws_handler(&venue);
  REQUIRE(server.listen(SockAddr::loopback_v4(port)));
  REQUIRE(run_until(reactor, [&] { return conn.state() == ConnState::Live; }, 3000));
  conn.close();
}

TEST_CASE("connection: wss:// with the fixture CA reports TlsHandshake") {
  Reactor reactor;
  VenueHandler venue;
  TlsContext sctx =
      TlsContext::server(tls_fixture("cert.pem").string(), tls_fixture("key.pem").string());
  HttpServer<TlsStream<PlainStream>> server(reactor, [&](TcpSocket&& s) {
    return TlsStream<PlainStream>(sctx, PlainStream(std::move(s)));
  });
  server.set_ws_handler(&venue);
  REQUIRE(server.listen(SockAddr::loopback_v4(0)));

  Events ev;
  ConnectionConfig cfg =
      fast_config("wss://localhost:" + std::to_string(server.port()) + "/stream");
  cfg.tls.ca_file = tls_fixture("cert.pem").string();
  Connection<TlsStream<PlainStream>, Events> conn(reactor, cfg, ev);
  ev.subscribe = [&] { conn.send_text("SUBSCRIBE"); };
  conn.connect();
  REQUIRE(run_until(
      reactor, [&] { return conn.state() == ConnState::Live && !ev.texts.empty(); }, 5000));
  CHECK(ev.states == std::vector<ConnState>{ConnState::Resolving,
                                            ConnState::Connecting,
                                            ConnState::TlsHandshake,
                                            ConnState::WsHandshake,
                                            ConnState::Authenticating,
                                            ConnState::Subscribing,
                                            ConnState::Live});

  SUBCASE("wrong CA -> Backoff") {
    conn.close();
    Events ev2;
    ConnectionConfig bad = cfg;
    bad.tls.ca_file.clear();  // system store does not trust the fixture
    Connection<TlsStream<PlainStream>, Events> c2(reactor, bad, ev2);
    c2.connect();
    REQUIRE(run_until(reactor, [&] { return ev2.count(ConnState::Backoff) == 1; }, 5000));
    CHECK(ev2.count(ConnState::TlsHandshake) == 1);
    c2.close();
  }
  SUBCASE("insecure mode accepts any certificate") {
    conn.close();
    Events ev3;
    ConnectionConfig ins = cfg;
    ins.tls.ca_file.clear();
    ins.tls.insecure = true;
    Connection<TlsStream<PlainStream>, Events> c3(reactor, ins, ev3);
    c3.connect();
    REQUIRE(run_until(reactor, [&] { return c3.state() == ConnState::Live; }, 5000));
    c3.close();
  }
}

TEST_CASE("connection: config validation") {
  Reactor reactor;
  Events ev;
  ConnectionConfig cfg;
  cfg.url = "http://x/";
  CHECK_THROWS_AS((Connection<PlainStream, Events>(reactor, cfg, ev)), std::invalid_argument);
  cfg.url = "ws://x/";
  cfg.stale_ms = 5000;
  cfg.dead_ms = 1000;
  CHECK_THROWS_AS((Connection<PlainStream, Events>(reactor, cfg, ev)), std::invalid_argument);
  CHECK(to_string(ConnState::Live) == "Live");
  CHECK(to_string(ConnState::Backoff) == "Backoff");
}
