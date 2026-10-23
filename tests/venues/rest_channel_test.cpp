// RestChannel: the order lane does not wait behind the control lane, a connection that dies takes
// only its own request with it, a connection that cannot be made fails its lane and looks the host
// up again, and no callback runs inside request().
#include "fastmm/venues/rest_channel.hpp"

#include "test_support.hpp"

#include "fastmm/net/http_server.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/tcp_socket.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;

namespace {

struct Reply {
  bool done = false;
  int status = 0;
  net::NetError error = net::NetError::None;
  std::string body;
  std::int64_t at_ns = 0;
};

net::HttpResponseCallback capture(Reply& out) {
  return [&out](const net::HttpResponse& r) {
    out.done = true;
    out.status = r.status;
    out.error = r.error;
    out.body = std::string(r.body);
    out.at_ns = net::Reactor::now_ns();
  };
}

template <class Pred>
bool run_until(net::Reactor& reactor, Pred pred, int timeout_ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > end) return false;
    reactor.run_once(1);
  }
  return true;
}

struct Server {
  explicit Server(net::Reactor& reactor)
      : http(reactor, [](net::TcpSocket&& s) { return net::PlainStream(std::move(s)); }) {
    http.route("GET", "/time", [](const net::HttpRequest&) {
      return net::HttpServerResponse::json(200, R"({"serverTime":1})");
    });
    http.route("DELETE", "/order", [](const net::HttpRequest& req) {
      return net::HttpServerResponse::json(200, "{\"id\":" + std::string(req.param("id")) + "}");
    });
    http.route(
        "GET", "/hang", [](const net::HttpRequest&) { return net::HttpServerResponse::none(); });
    http.route("GET", "/close", [](const net::HttpRequest&) {
      net::HttpServerResponse r = net::HttpServerResponse::text(200, "bye");
      r.close = true;
      return r;
    });
    REQUIRE(http.listen(net::SockAddr::loopback_v4(0)));
  }
  std::string url() const { return "http://127.0.0.1:" + std::to_string(http.port()); }

  net::HttpServer<net::PlainStream> http;
};

RestChannelConfig config(const std::string& url, std::uint32_t timeout_ms) {
  RestChannelConfig c;
  c.base_url = url;
  c.timeout_ms = timeout_ms;
  c.max_queue = 64;
  return c;
}

}  // namespace

TEST_CASE("venues.rest_channel: a cancel does not wait behind the control lane") {
  net::Reactor reactor;
  Server server(reactor);
  RestChannel rest(reactor, config(server.url(), 1500));

  // A control request the venue sits on, and a snapshot queued behind it.
  Reply hang;
  Reply snapshot;
  REQUIRE(rest.request("GET", "/hang", {}, {}, capture(hang)));
  REQUIRE(rest.request("GET", "/time", {}, {}, capture(snapshot)));
  Reply cancel;
  const std::int64_t sent = net::Reactor::now_ns();
  REQUIRE(rest.request("DELETE", "/order?id=7", {}, {}, capture(cancel), RestLane::Orders));
  REQUIRE(run_until(reactor, [&] { return cancel.done; }, 1000));
  CHECK(cancel.status == 200);
  CHECK(cancel.body == R"({"id":7})");
  CHECK(cancel.at_ns - sent < 500'000'000);
  CHECK_FALSE(hang.done);  // still with the venue
  CHECK_FALSE(snapshot.done);
  CHECK(rest.queued(RestLane::Control) == 2);
  CHECK(rest.queued(RestLane::Orders) == 0);
  CHECK(rest.connected(RestLane::Orders));

  // The hung request times out; the one behind it was never written and goes out on a new
  // connection instead of failing with it.
  REQUIRE(run_until(reactor, [&] { return snapshot.done; }, 4000));
  CHECK(hang.error == net::NetError::Timeout);
  CHECK(snapshot.error == net::NetError::None);
  CHECK(snapshot.status == 200);
  CHECK(rest.connects() == 3);  // control, orders, and control again
}

TEST_CASE("venues.rest_channel: a server that closes after a reply loses no queued request") {
  net::Reactor reactor;
  Server server(reactor);
  RestChannel rest(reactor, config(server.url(), 1000));
  std::vector<Reply> replies(4);
  REQUIRE(rest.request("GET", "/close", {}, {}, capture(replies[0])));
  REQUIRE(rest.request("GET", "/time", {}, {}, capture(replies[1])));
  REQUIRE(rest.request("GET", "/close", {}, {}, capture(replies[2])));
  REQUIRE(rest.request("GET", "/time", {}, {}, capture(replies[3])));
  REQUIRE(run_until(reactor, [&] { return replies[3].done; }, 3000));
  for (const Reply& r : replies) {
    CHECK(r.done);
    CHECK(r.error == net::NetError::None);
    CHECK(r.status == 200);
  }
  CHECK(rest.connects() == 3);
  CHECK(server.http.stats().requests == 4);

  // A request made from the callback of a closing answer waits for the next connection; one made
  // from the callback of an ordinary answer goes out on the same one.
  Reply chained_after_close;
  Reply chained;
  Reply first;
  REQUIRE(rest.request("GET", "/close", {}, {}, [&](const net::HttpResponse& r) {
    capture(first)(r);
    CHECK(rest.request("GET", "/time", {}, {}, [&](const net::HttpResponse& r2) {
      capture(chained_after_close)(r2);
      CHECK(rest.request("GET", "/time", {}, {}, capture(chained)));
    }));
  }));
  REQUIRE(run_until(reactor, [&] { return chained.done; }, 3000));
  CHECK(first.status == 200);
  CHECK(chained_after_close.status == 200);
  CHECK(chained.status == 200);
  CHECK(rest.connects() == 4);  // the /close on the open one, a new one for both chained requests
}

TEST_CASE(
    "venues.rest_channel: a connection that cannot be made fails its lane, then looks again") {
  net::Reactor reactor;
  // A port nobody listens on: bind one, read its number, close it.
  std::uint16_t port = 0;
  {
    net::TcpSocket probe = net::TcpSocket::listen(net::SockAddr::loopback_v4(0));
    REQUIRE(probe.valid());
    port = probe.local_addr()->port();
  }
  RestChannel rest(reactor, config("http://127.0.0.1:" + std::to_string(port), 1000));
  CHECK(rest.lookups() == 1);  // made with the channel
  reactor.run_once(0);         // and answered

  Reply a;
  Reply b;
  Reply order;
  bool inside = false;
  auto check_outside = [&](Reply& out) {
    return [&inside, &out](const net::HttpResponse& r) {
      CHECK_FALSE(inside);  // never from inside request()
      out.done = true;
      out.error = r.error;
    };
  };
  inside = true;
  REQUIRE(rest.request("GET", "/time", {}, {}, check_outside(a)));
  REQUIRE(rest.request("GET", "/time", {}, {}, check_outside(b)));
  inside = false;
  REQUIRE(run_until(reactor, [&] { return a.done && b.done; }, 2000));
  CHECK(a.error != net::NetError::None);
  CHECK(b.error != net::NetError::None);  // behind a connection that never came up
  CHECK(rest.queued(RestLane::Control) == 0);

  // The next request resolves the host again before it connects.
  REQUIRE(rest.request("DELETE", "/order?id=1", {}, {}, check_outside(order), RestLane::Orders));
  REQUIRE(run_until(reactor, [&] { return order.done; }, 2000));
  CHECK(order.error != net::NetError::None);
  CHECK(rest.lookups() == 2);
}

TEST_CASE("venues.rest_channel: reset cancels what waits and what is in flight") {
  net::Reactor reactor;
  Server server(reactor);
  RestChannel rest(reactor, config(server.url(), 5000));
  Reply hang;
  Reply behind;
  Reply order;
  REQUIRE(rest.request("GET", "/hang", {}, {}, capture(hang)));
  REQUIRE(rest.request("GET", "/time", {}, {}, capture(behind)));
  REQUIRE(rest.request("GET", "/hang", {}, {}, capture(order), RestLane::Orders));
  REQUIRE(run_until(reactor, [&] { return server.http.stats().requests == 2; }, 1000));
  rest.reset();
  CHECK(hang.error == net::NetError::Canceled);
  CHECK(behind.error == net::NetError::Canceled);
  CHECK(order.error == net::NetError::Canceled);
  CHECK(rest.queued(RestLane::Control) == 0);
  CHECK(rest.queued(RestLane::Orders) == 0);

  // Still usable.
  Reply after;
  REQUIRE(rest.request("GET", "/time", {}, {}, capture(after)));
  REQUIRE(run_until(reactor, [&] { return after.done; }, 1000));
  CHECK(after.status == 200);
}

TEST_CASE("venues.rest_channel: a full lane refuses, the other lane still takes requests") {
  net::Reactor reactor;
  Server server(reactor);
  RestChannelConfig c = config(server.url(), 5000);
  c.max_queue = 2;
  RestChannel rest(reactor, c);
  Reply r1;
  Reply r2;
  Reply r3;
  Reply o1;
  CHECK(rest.request("GET", "/hang", {}, {}, capture(r1)));
  CHECK(rest.request("GET", "/time", {}, {}, capture(r2)));
  CHECK_FALSE(rest.request("GET", "/time", {}, {}, capture(r3)));
  CHECK(rest.request("DELETE", "/order?id=2", {}, {}, capture(o1), RestLane::Orders));
  REQUIRE(run_until(reactor, [&] { return o1.done; }, 1000));
  CHECK(o1.status == 200);
  CHECK_FALSE(r3.done);
}
