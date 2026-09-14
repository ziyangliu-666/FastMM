#include "net_test_util.hpp"

#include "fastmm/net/http_client.hpp"
#include "fastmm/net/http_message.hpp"
#include "fastmm/net/http_server.hpp"
#include "fastmm/net/tls_stream.hpp"

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

using namespace fastmm::net;
using namespace fastmm::net::test;

namespace {

void feed(RecvBuffer& rx, std::string_view data) {
  auto w = rx.writable();
  REQUIRE(w.size() >= data.size());
  std::memcpy(w.data(), data.data(), data.size());
  rx.commit(data.size());
}

// Feeds `wire` in `chunk` sized pieces and returns the completed response (body copied).
struct Parsed {
  int status = 0;
  std::string body;
  std::string content_type;
  std::size_t consumed = 0;
  bool close = false;
  bool complete = false;
  std::string error;
};

Parsed parse_in_chunks(std::string_view wire,
                       std::size_t chunk,
                       bool head_request = false,
                       bool eof_at_end = false) {
  RecvBuffer rx(64 * 1024);
  detail::HttpResponseParser parser;
  parser.begin(head_request);
  Parsed out;
  for (std::size_t off = 0; off < wire.size(); off += chunk) {
    feed(rx, wire.substr(off, chunk));
    HttpResponse resp;
    std::size_t consume = 0;
    const auto r = parser.parse(rx, resp, consume);
    if (r == detail::HttpResponseParser::Result::Invalid) {
      out.error = parser.error();
      return out;
    }
    if (r == detail::HttpResponseParser::Result::Complete) {
      out.status = resp.status;
      out.body = std::string(resp.body);
      out.content_type = std::string(resp.header("content-type"));
      out.consumed = consume;
      out.close = parser.connection_close();
      out.complete = true;
      return out;
    }
  }
  if (eof_at_end) {
    HttpResponse resp;
    std::size_t consume = 0;
    const auto r = parser.on_eof(rx, resp, consume);
    if (r == detail::HttpResponseParser::Result::Complete) {
      out.status = resp.status;
      out.body = std::string(resp.body);
      out.consumed = consume;
      out.close = parser.connection_close();
      out.complete = true;
    } else {
      out.error = parser.error();
    }
  }
  return out;
}

}  // namespace

TEST_CASE("http_message: request and response head parsing") {
  HttpRequestHead req;
  const std::string r =
      "POST /api/v3/order?symbol=BTCUSDT&side=BUY HTTP/1.1\r\nHost: x\r\nContent-Length: "
      "3\r\nX-MBX-APIKEY: k\r\n\r\nabc";
  REQUIRE(parse_request_head(r, req) == HttpParseStatus::Ok);
  CHECK(req.method == "POST");
  CHECK(req.path == "/api/v3/order");
  CHECK(req.query == "symbol=BTCUSDT&side=BUY");
  CHECK(req.version == "HTTP/1.1");
  CHECK(req.headers.count == 3);
  CHECK(req.headers.get("content-length") == "3");
  CHECK(req.headers.get("x-mbx-apikey") == "k");
  CHECK(req.head_len == r.size() - 3);
  CHECK(query_param(req.query, "side") == "BUY");
  CHECK(query_param(req.query, "symbol") == "BTCUSDT");
  CHECK(query_param(req.query, "nope").empty());

  CHECK(parse_request_head("GET / HTTP/1.1\r\nHost", req) == HttpParseStatus::Incomplete);
  CHECK(parse_request_head("GET /\r\n\r\n", req) == HttpParseStatus::Invalid);
  CHECK(parse_request_head("GET / HTTP/1.1\r\nNoColon\r\n\r\n", req) == HttpParseStatus::Invalid);
  std::string many = "GET / HTTP/1.1\r\n";
  for (int i = 0; i < 40; ++i) many += "H" + std::to_string(i) + ": v\r\n";
  many += "\r\n";
  CHECK(parse_request_head(many, req) == HttpParseStatus::TooManyHeaders);

  HttpResponseHead resp;
  REQUIRE(parse_response_head("HTTP/1.1 429 Too Many Requests\r\nRetry-After: 3\r\n\r\n", resp) ==
          HttpParseStatus::Ok);
  CHECK(resp.status == 429);
  CHECK(resp.reason == "Too Many Requests");
  CHECK(resp.headers.get("retry-after") == "3");
  REQUIRE(parse_response_head("HTTP/1.1 204 \r\n\r\n", resp) == HttpParseStatus::Ok);
  CHECK(resp.status == 204);
  CHECK(parse_response_head("HTTP/1.1 2x0 OK\r\n\r\n", resp) == HttpParseStatus::Invalid);
  CHECK(parse_response_head("HTTP/1.1 200", resp) == HttpParseStatus::Incomplete);

  CHECK(iequals("Content-Type", "content-type"));
  CHECK_FALSE(iequals("a", "ab"));
  CHECK(header_has_token("keep-alive, Upgrade", "upgrade"));
  CHECK_FALSE(header_has_token("keep-alive", "upgrade"));
  std::size_t n = 0;
  CHECK(parse_content_length(" 42 ", n));
  CHECK(n == 42);
  CHECK_FALSE(parse_content_length("4x", n));
  CHECK_FALSE(parse_content_length("", n));
}

TEST_CASE("http_parser: body framing vectors") {
  for (std::size_t chunk : {1u, 3u, 7u, 4096u}) {
    CAPTURE(chunk);
    SUBCASE("Content-Length") {
      auto p = parse_in_chunks(
          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
          "13\r\n\r\n{\"a\":1,\"b\":2}",
          chunk);
      REQUIRE(p.complete);
      CHECK(p.status == 200);
      CHECK(p.body == "{\"a\":1,\"b\":2}");
      CHECK(p.content_type == "application/json");
      CHECK_FALSE(p.close);
    }
    SUBCASE("chunked with extension and trailers") {
      const std::string wire =
          "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
          "4\r\nWiki\r\n"
          "6;ext=1\r\npedia \r\n"
          "E\r\nin \r\n\r\nchunks.\r\n"
          "0\r\nX-Checksum: abc\r\nX-Other: 1\r\n\r\n";
      auto p = parse_in_chunks(wire, chunk);
      REQUIRE(p.complete);
      CHECK(p.body == "Wikipedia in \r\n\r\nchunks.");
      CHECK(p.consumed == wire.size());
      CHECK_FALSE(p.close);
    }
    SUBCASE("chunked without trailers followed by the next response") {
      const std::string wire =
          "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\nHTTP/1.1 200 "
          "OK\r\n";
      auto p = parse_in_chunks(wire, chunk);
      REQUIRE(p.complete);
      CHECK(p.body == "abc");
      CHECK(p.consumed == wire.size() - std::strlen("HTTP/1.1 200 OK\r\n"));
    }
    SUBCASE("close-delimited body") {
      auto p = parse_in_chunks("HTTP/1.1 200 OK\r\nServer: old\r\n\r\nuntil-close-body",
                               chunk,
                               false,
                               /*eof_at_end=*/true);
      REQUIRE(p.complete);
      CHECK(p.body == "until-close-body");
      CHECK(p.close);
    }
    SUBCASE("100-continue is skipped") {
      auto p = parse_in_chunks(
          "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok",
          chunk);
      REQUIRE(p.complete);
      CHECK(p.status == 201);
      CHECK(p.body == "ok");
    }
    SUBCASE("204 and HEAD carry no body") {
      auto p = parse_in_chunks("HTTP/1.1 204 No Content\r\nContent-Length: 999\r\n\r\n", chunk);
      REQUIRE(p.complete);
      CHECK(p.body.empty());
      auto h = parse_in_chunks(
          "HTTP/1.1 200 OK\r\nContent-Length: 999\r\n\r\n", chunk, /*head_request=*/true);
      REQUIRE(h.complete);
      CHECK(h.body.empty());
    }
    SUBCASE("Connection: close is reported") {
      auto p = parse_in_chunks("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 0\r\n\r\n",
                               chunk);
      REQUIRE(p.complete);
      CHECK(p.close);
      auto q = parse_in_chunks("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n", chunk);
      REQUIRE(q.complete);
      CHECK(q.close);
    }
  }
  SUBCASE("malformed chunk size / missing CRLF / bad length") {
    CHECK(parse_in_chunks("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", 4096)
              .error == "bad chunk size");
    CHECK(parse_in_chunks("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcXX", 4096)
              .error == "missing CRLF after chunk");
    CHECK(parse_in_chunks("HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n", 4096).error ==
          "bad Content-Length");
    CHECK(parse_in_chunks("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nab", 4096, false, true)
              .error == "connection closed mid-response");
  }
}

// ------------------------------------------------------------------------- client <-> server

namespace {

template <class Server>
void install_routes(Server& server) {
  server.route("GET", "/api/v3/time", [](const HttpRequest&) {
    return HttpServerResponse::json(200, "{\"serverTime\":1700000000000}");
  });
  server.route("POST", "/api/v3/order", [](const HttpRequest& req) {
    HttpServerResponse r =
        HttpServerResponse::json(200,
                                 "{\"body\":\"" + std::string(req.body) + "\",\"key\":\"" +
                                     std::string(req.header("X-MBX-APIKEY")) + "\"}");
    r.headers.emplace_back("X-MBX-USED-WEIGHT-1M", "10");
    return r;
  });
  server.route("DELETE", "/api/v3/order", [](const HttpRequest& req) {
    return HttpServerResponse::json(200,
                                    "{\"symbol\":\"" + std::string(req.param("symbol")) +
                                        "\",\"orderId\":" + std::string(req.param("orderId")) +
                                        "}");
  });
  server.route("GET", "/close", [](const HttpRequest&) {
    HttpServerResponse r = HttpServerResponse::text(200, "bye");
    r.close = true;
    return r;
  });
  server.route("GET", "/hang", [](const HttpRequest&) { return HttpServerResponse::none(); });
  server.route("GET", "/teapot", [](const HttpRequest&) {
    return HttpServerResponse::text(418, "short and stout");
  });
}

PlainStream connect_plain(std::uint16_t port) {
  TcpSocket s;
  REQUIRE(s.connect(SockAddr::loopback_v4(port)) != ConnectStatus::Error);
  return PlainStream(std::move(s));
}

struct Captured {
  int status = 0;
  NetError error = NetError::None;
  std::string body;
  std::string weight;
  bool done = false;
};

HttpResponseCallback capture(Captured& c) {
  return [&c](const HttpResponse& r) {
    c.status = r.status;
    c.error = r.error;
    c.body = std::string(r.body);
    c.weight = std::string(r.header("x-mbx-used-weight-1m"));
    c.done = true;
  };
}

template <class Client, class Server, class MakeStream>
void client_scenario(Reactor& reactor, Server& server, MakeStream make_stream) {
  HttpClientConfig cfg;
  cfg.timeout_ms = 200;
  Client client(reactor, make_stream(), "localhost", server.port(), false, cfg);
  REQUIRE(client.start());

  SUBCASE("GET / POST / DELETE pipeline through the FIFO on one connection") {
    Captured g, p, d;
    REQUIRE(client.request("GET", "/api/v3/time", "", "", capture(g)));
    REQUIRE(client.request(
        "POST",
        "/api/v3/order",
        "X-MBX-APIKEY: key123\r\nContent-Type: application/x-www-form-urlencoded\r\n",
        "symbol=BTCUSDT&side=BUY",
        capture(p)));
    REQUIRE(
        client.request("DELETE", "/api/v3/order?symbol=ETHUSDT&orderId=42", "", "", capture(d)));
    CHECK(client.queued() == 3);
    REQUIRE(run_until(reactor, [&] { return g.done && p.done && d.done; }));
    CHECK(g.status == 200);
    CHECK(g.body == "{\"serverTime\":1700000000000}");
    CHECK(p.status == 200);
    CHECK(p.body == "{\"body\":\"symbol=BTCUSDT&side=BUY\",\"key\":\"key123\"}");
    CHECK(p.weight == "10");
    CHECK(d.status == 200);
    CHECK(d.body == "{\"symbol\":\"ETHUSDT\",\"orderId\":42}");
    CHECK(server.stats().accepted == 1);
    CHECK(server.stats().requests == 3);
    CHECK(client.stats().responses == 3);
    CHECK(client.is_ready());
  }
  SUBCASE("keep-alive: 100 sequential requests reuse one socket") {
    int done = 0;
    std::function<void()> next = [&] {
      client.request("GET", "/api/v3/time", "", "", [&](const HttpResponse& r) {
        CHECK(r.status == 200);
        if (++done < 100) next();
      });
    };
    next();
    REQUIRE(run_until(reactor, [&] { return done == 100; }));
    CHECK(server.stats().accepted == 1);
    CHECK(server.stats().requests == 100);
  }
  SUBCASE("404 and non-2xx are delivered, not errors") {
    Captured c;
    REQUIRE(client.request("GET", "/nope", "", "", capture(c)));
    REQUIRE(run_until(reactor, [&] { return c.done; }));
    CHECK(c.status == 404);
    CHECK(c.error == NetError::None);
    Captured t;
    REQUIRE(client.request("GET", "/teapot", "", "", capture(t)));
    REQUIRE(run_until(reactor, [&] { return t.done; }));
    CHECK(t.status == 418);
    CHECK(t.body == "short and stout");
  }
  SUBCASE("timeout fires and cancels the rest of the queue") {
    Captured h, q;
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(client.request("GET", "/hang", "", "", capture(h)));
    REQUIRE(client.request("GET", "/api/v3/time", "", "", capture(q)));
    REQUIRE(run_until(reactor, [&] { return h.done && q.done; }, 3000));
    CHECK(h.error == NetError::Timeout);
    CHECK(q.error == NetError::Canceled);
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(150));
    CHECK(client.stats().timeouts == 1);
    CHECK(client.state() == HttpClientState::Closed);
    CHECK_FALSE(client.request("GET", "/api/v3/time", "", "", capture(q)));
  }
  SUBCASE("server-side Connection: close ends the session after the response") {
    Captured c, after;
    REQUIRE(client.request("GET", "/close", "", "", capture(c)));
    REQUIRE(client.request("GET", "/api/v3/time", "", "", capture(after)));
    REQUIRE(run_until(reactor, [&] { return c.done && after.done; }));
    CHECK(c.status == 200);
    CHECK(c.body == "bye");
    CHECK(after.error == NetError::Closed);
    CHECK(client.state() == HttpClientState::Closed);
  }
  SUBCASE("queue limit") {
    for (std::size_t i = 0; i < cfg.max_queue; ++i)
      REQUIRE(client.request("GET", "/hang", "", "", nullptr));
    CHECK_FALSE(client.request("GET", "/hang", "", "", nullptr));
    client.close();
    CHECK(client.queued() == 0);
  }
}

}  // namespace

FASTMM_BACKEND_TEST("http: client/server over PlainStream", test_http_1) {
  Reactor reactor(backend);
  HttpServer<PlainStream> server(reactor, [](TcpSocket&& s) { return PlainStream(std::move(s)); });
  install_routes(server);
  REQUIRE(server.listen(SockAddr::loopback_v4(0)));
  client_scenario<HttpClient<PlainStream>>(
      reactor, server, [&] { return connect_plain(server.port()); });
}

FASTMM_BACKEND_TEST("http: client/server over TlsStream", test_http_2) {
  Reactor reactor(backend);
  TlsContext sctx =
      TlsContext::server(tls_fixture("cert.pem").string(), tls_fixture("key.pem").string());
  TlsContext cctx;
  cctx.set_ca_file(tls_fixture("cert.pem").string());
  HttpServer<TlsStream<PlainStream>> server(reactor, [&](TcpSocket&& s) {
    return TlsStream<PlainStream>(sctx, PlainStream(std::move(s)));
  });
  install_routes(server);
  REQUIRE(server.listen(SockAddr::loopback_v4(0)));
  client_scenario<HttpClient<TlsStream<PlainStream>>>(reactor, server, [&] {
    return TlsStream<PlainStream>(cctx, connect_plain(server.port()), "localhost");
  });
}

FASTMM_BACKEND_TEST("http: server rejects malformed and oversized requests", test_http_3) {
  Reactor reactor(backend);
  HttpServerConfig cfg;
  cfg.max_body_bytes = 16;
  HttpServer<PlainStream> server(
      reactor, [](TcpSocket&& s) { return PlainStream(std::move(s)); }, cfg);
  install_routes(server);
  REQUIRE(server.listen(SockAddr::loopback_v4(0)));

  auto raw_exchange = [&](std::string_view wire) {
    TcpSocket s;
    REQUIRE(s.connect(SockAddr::loopback_v4(server.port())) != ConnectStatus::Error);
    std::string got;
    bool closed = false;
    class Collector final : public IoHandler {
     public:
      Collector(TcpSocket& sock, std::string& out, bool& c) : sock_(sock), out_(out), closed_(c) {}
      void on_readable() override {
        std::byte buf[1024];
        for (;;) {
          auto r = sock_.read(buf);
          if (r.bytes > 0) out_.append(reinterpret_cast<const char*>(buf), r.bytes);
          if (r.closed) closed_ = true;
          if (r.bytes == 0) return;
        }
      }
      void on_writable() override {}
      void on_error(int) override { closed_ = true; }

     private:
      TcpSocket& sock_;
      std::string& out_;
      bool& closed_;
    } collector(s, got, closed);
    REQUIRE(reactor.add(s.fd(), collector, IoEvent::Read));
    REQUIRE(run_until(reactor, [&] { return s.peer_addr().has_value(); }));
    REQUIRE(s.write(bytes(wire)).ok());
    run_until(reactor, [&] { return closed; });
    reactor.remove(s.fd());
    return got;
  };

  CHECK(raw_exchange("GARBAGE\r\n\r\n").starts_with("HTTP/1.1 400"));
  CHECK(raw_exchange("POST /api/v3/order HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\n")
            .starts_with("HTTP/1.1 413"));
  CHECK(raw_exchange("GET /api/v3/time HTTP/1.0\r\n\r\n")
            .starts_with("HTTP/1.1 200"));  // 1.0 => close
  REQUIRE(run_until(reactor, [&] { return server.http_connections() == 0; }));
}
