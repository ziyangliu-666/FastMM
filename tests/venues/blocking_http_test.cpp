// BlockingHttp keeps its connection between requests, notices a server that closed it, and sends
// an idempotent request again when the kept connection was gone under it.
#include "fastmm/venues/blocking_http.hpp"

#include "test_support.hpp"

#include "fastmm/net/http_server.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/tcp_socket.hpp"
#include "fastmm/venues/blocking_control.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;

namespace {

// An HTTP server on its own thread; `close_next` makes the next answer close its connection.
class Server {
 public:
  Server() : http_(reactor_, [](net::TcpSocket&& s) { return net::PlainStream(std::move(s)); }) {
    http_.route("GET", "/time", [this](const net::HttpRequest&) {
      net::HttpServerResponse r = net::HttpServerResponse::json(200, R"({"serverTime":1})");
      r.close = close_next_.exchange(false);
      return r;
    });
    http_.route("DELETE", "/openOrders", [](const net::HttpRequest& req) {
      return net::HttpServerResponse::json(200, "[\"" + std::string(req.param("symbol")) + "\"]");
    });
    http_.route("POST", "/countdown", [](const net::HttpRequest&) {
      return net::HttpServerResponse::json(200, "{}");
    });
    for (const char* method : {"GET", "POST"}) {
      http_.route(method, "/vanish", [this](const net::HttpRequest&) {
        http_.close_all();  // deferred: the connection goes without an answer
        return net::HttpServerResponse::none();
      });
    }
    REQUIRE(http_.listen(net::SockAddr::loopback_v4(0)));
    thread_ = std::thread([this] { reactor_.run(stop_); });
  }
  ~Server() {
    stop_.store(true);
    reactor_.wake();
    thread_.join();
  }
  std::string url() const { return "http://127.0.0.1:" + std::to_string(http_.port()); }
  std::uint64_t accepted() {
    std::atomic<std::uint64_t> n{~0ULL};
    reactor_.post([&] { n.store(http_.stats().accepted); });
    while (n.load() == ~0ULL) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return n.load();
  }
  // Drops every open connection without an answer (the server side of an idle timeout).
  void drop_connections() {
    std::atomic<bool> done{false};
    reactor_.post([&] {
      http_.close_all();
      done.store(true);
    });
    while (!done.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  void close_next() { close_next_.store(true); }

 private:
  net::Reactor reactor_;
  net::HttpServer<net::PlainStream> http_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> close_next_{false};
  std::thread thread_;
};

}  // namespace

TEST_CASE("venues.blocking_http: one connection carries a kill switch's requests") {
  Server server;
  BlockingControl control("test", server.url(), BlockingHttpOptions{});
  const std::vector<std::string> symbols = {"BTCUSDT", "ETHUSDT", "SOLUSDT", "XRPUSDT"};
  const bool ok = control.per_target(
      "cancel-all",
      std::span<const std::string>(symbols),
      [](const std::string& s) { return s; },
      [](const std::string& s, BlockingRequest& q) {
        q.method = "DELETE";
        q.target = "/openOrders?symbol=" + s;
        return true;
      },
      [](const HttpReply& r, std::string&) { return r.ok(); });
  CHECK(ok);
  CHECK(server.accepted() == 1);
}

TEST_CASE("venues.blocking_http: a connection the server closed is not reused") {
  Server server;
  BlockingHttp http(server.url());
  CHECK(http.get("/time").ok());
  CHECK(http.get("/time").ok());
  CHECK(http.connects() == 1);

  server.close_next();  // the answer says Connection: close
  CHECK(http.get("/time").ok());
  CHECK(http.get("/time").ok());
  CHECK(http.connects() == 2);

  // Closed by the server while idle: seen before the next request goes out.
  server.drop_connections();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(http.request("POST", "/countdown").ok());
  CHECK(http.connects() == 3);
  CHECK(server.accepted() == 3);
}

TEST_CASE("venues.blocking_http: only an idempotent request goes again when the connection died") {
  Server server;
  BlockingHttp http(server.url());
  CHECK(http.get("/time").ok());
  CHECK(http.connects() == 1);
  // The server reads the request and drops the connection without an answer. On the kept
  // connection a GET is sent once more on a new one (where it is dropped again: no third try).
  const HttpReply g = http.get("/vanish");
  CHECK_FALSE(g.ok());
  CHECK(http.connects() == 2);
  // A POST is not: it may have been carried out.
  CHECK(http.get("/time").ok());
  CHECK(http.connects() == 3);
  const HttpReply p = http.request("POST", "/vanish");
  CHECK_FALSE(p.ok());
  CHECK(http.connects() == 3);
  CHECK(server.accepted() == 3);
}
