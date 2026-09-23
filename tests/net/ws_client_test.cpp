#include "fastmm/net/ws_client.hpp"

#include "net_test_util.hpp"

#include "fastmm/net/http_server.hpp"
#include "fastmm/net/tls_stream.hpp"
#include "fastmm/net/ws_server.hpp"

#include <sys/socket.h>

#include <memory>
#include <string>
#include <vector>

using namespace fastmm::net;
using namespace fastmm::net::test;

namespace {

struct ClientEvents {
  bool open = false;
  std::vector<std::string> texts;
  std::vector<std::string> binaries;
  std::vector<std::int64_t> rx_ts;
  int pings = 0;
  int pongs = 0;
  bool closed = false;
  std::uint16_t close_code = 0;
  std::string close_reason;
  bool errored = false;
  NetError error = NetError::None;
  std::string error_detail;

  std::vector<WsProgress> progress;

  void on_ws_progress(WsProgress p) { progress.push_back(p); }
  void on_ws_open() { open = true; }
  void on_ws_text(std::string_view s, std::int64_t ts) {
    texts.emplace_back(s);
    rx_ts.push_back(ts);
  }
  void on_ws_binary(std::span<const std::byte> b, std::int64_t) { binaries.emplace_back(sv(b)); }
  void on_ws_ping(std::span<const std::byte>) { ++pings; }
  void on_ws_pong(std::span<const std::byte>) { ++pongs; }
  void on_ws_close(std::uint16_t code, std::string_view reason) {
    closed = true;
    close_code = code;
    close_reason = std::string(reason);
  }
  void on_ws_error(NetError e, std::string_view d) {
    errored = true;
    error = e;
    error_detail = std::string(d);
  }
  bool finished() const { return closed || errored; }
};

// Echo server handler that records what it sees and exposes the live session.
class EchoHandler final : public WsSessionHandler {
 public:
  bool accept_upgrade(std::string_view path, std::string_view) override {
    return path != "/forbidden";
  }
  void on_open(WsSession& s) override {
    session = &s;
    ++opened;
    last_path = std::string(s.path());
    last_query = std::string(s.query());
  }
  void on_text(WsSession& s, std::string_view t) override {
    texts.emplace_back(t);
    if (echo) s.send_text(t);
  }
  void on_binary(WsSession& s, std::span<const std::byte> b) override {
    if (echo) s.send_binary(b);
  }
  void on_pong(WsSession&, std::span<const std::byte>) override { ++pongs; }
  void on_close(WsSession&, std::uint16_t code, std::string_view) override {
    session = nullptr;
    ++closed;
    last_close_code = code;
  }
  void on_error(WsSession&, NetError e, std::string_view) override {
    session = nullptr;
    ++errors;
    last_error = e;
  }

  WsSession* session = nullptr;
  bool echo = true;
  int opened = 0;
  int closed = 0;
  int errors = 0;
  int pongs = 0;
  std::uint16_t last_close_code = 0;
  NetError last_error = NetError::None;
  std::string last_path;
  std::string last_query;
  std::vector<std::string> texts;
};

using PlainServer = HttpServer<PlainStream>;
using TlsServer = HttpServer<TlsStream<PlainStream>>;
using PlainClient = WsClient<PlainStream, ClientEvents>;
using TlsClient = WsClient<TlsStream<PlainStream>, ClientEvents>;

PlainStream connect_plain(std::uint16_t port) {
  TcpSocket s;
  REQUIRE(s.connect(SockAddr::loopback_v4(port)) != ConnectStatus::Error);
  return PlainStream(std::move(s));
}

// Generic echo scenario run for both stream types.
template <class Client, class Server, class MakeClientStream>
void echo_scenario(Reactor& reactor,
                   Server& server,
                   EchoHandler& handler,
                   MakeClientStream make_stream) {
  ClientEvents ev;
  WsClientConfig cfg;
  cfg.recv_capacity = 256 * 1024;
  Client client(reactor, make_stream(), ev, cfg);
  REQUIRE(client.start("localhost", server.port(), false, "/ws?stream=btcusdt"));
  REQUIRE(run_until(reactor, [&] { return ev.open; }));
  CHECK(client.is_open());
  REQUIRE(ev.progress.size() == 2);
  CHECK(ev.progress[0] == WsProgress::TcpConnected);
  CHECK(ev.progress[1] == WsProgress::UpgradeSent);
  CHECK(handler.last_path == "/ws");
  CHECK(handler.last_query == "stream=btcusdt");
  CHECK(server.ws_sessions() == 1);

  SUBCASE("text and binary echo, including a 100 KiB message") {
    REQUIRE(client.send_text("hello"));
    std::string big(100 * 1024, 'B');
    REQUIRE(client.send_binary(bytes(big)));
    REQUIRE(run_until(reactor, [&] { return ev.texts.size() == 1 && ev.binaries.size() == 1; }));
    CHECK(ev.texts[0] == "hello");
    CHECK(ev.binaries[0] == big);
    CHECK(ev.rx_ts[0] > 0);
    CHECK(client.stats().frames_tx == 2);
    CHECK(client.stats().frames_rx == 2);
    CHECK(client.stats().drops == 0);
  }
  SUBCASE("many small messages keep order") {
    for (int i = 0; i < 500; ++i) REQUIRE(client.send_text("m" + std::to_string(i)));
    REQUIRE(run_until(reactor, [&] { return ev.texts.size() == 500; }));
    for (int i = 0; i < 500; ++i)
      CHECK(ev.texts[static_cast<std::size_t>(i)] == "m" + std::to_string(i));
  }
  SUBCASE("corked frames go out together at uncork, in order") {
    const std::uint64_t before = client.stats().bytes_tx;
    client.cork();
    for (int i = 0; i < 50; ++i) REQUIRE(client.send_text("c" + std::to_string(i)));
    CHECK(client.stats().bytes_tx == before);  // nothing written while corked
    REQUIRE(client.uncork());
    CHECK(client.stats().frames_tx == 50);
    CHECK(client.stats().bytes_tx > before);
    REQUIRE(run_until(reactor, [&] { return ev.texts.size() == 50; }));
    for (int i = 0; i < 50; ++i)
      CHECK(ev.texts[static_cast<std::size_t>(i)] == "c" + std::to_string(i));
  }
  SUBCASE("ping/pong both directions") {
    REQUIRE(client.send_ping(bytes("p")));
    REQUIRE(run_until(reactor, [&] { return ev.pongs == 1; }));
    handler.session->send_ping(bytes("server-ping"));
    REQUIRE(run_until(reactor, [&] { return ev.pings == 1 && handler.pongs == 1; }));
  }
  SUBCASE("server-sent fragments with an interleaved ping are reassembled") {
    WsSession& s = *handler.session;
    REQUIRE(s.send_raw(WsOpcode::Text, false, bytes("frag")));
    REQUIRE(s.send_raw(WsOpcode::Ping, true, bytes("mid")));
    REQUIRE(s.send_raw(WsOpcode::Continuation, false, bytes("men")));
    REQUIRE(s.send_raw(WsOpcode::Continuation, true, bytes("ted")));
    REQUIRE(run_until(reactor, [&] { return ev.texts.size() == 1; }));
    CHECK(ev.texts[0] == "fragmented");
    CHECK(ev.pings == 1);
  }
  SUBCASE("client-initiated close handshake") {
    REQUIRE(client.send_close(WsCloseCode::Normal, "done"));
    CHECK(client.state() == WsState::Closing);
    REQUIRE(run_until(reactor, [&] { return ev.closed; }));
    CHECK(ev.close_code == 1000);
    CHECK(client.state() == WsState::Closed);
    REQUIRE(run_until(reactor, [&] { return handler.closed == 1; }));
    CHECK(handler.last_close_code == 1000);
    REQUIRE(run_until(reactor, [&] { return server.ws_sessions() == 0; }));
  }
  SUBCASE("server-initiated close handshake") {
    handler.session->send_close(WsCloseCode::GoingAway, "maintenance");
    REQUIRE(run_until(reactor, [&] { return ev.closed; }));
    CHECK(ev.close_code == 1001);
    CHECK(ev.close_reason == "maintenance");
    REQUIRE(run_until(reactor, [&] { return handler.closed == 1; }));
  }
  SUBCASE("server drops the socket: client reports an error") {
    handler.session->close_abrupt();
    REQUIRE(run_until(reactor, [&] { return ev.errored; }));
    CHECK(ev.error == NetError::Closed);
    CHECK_FALSE(ev.closed);
  }
  SUBCASE("protocol violation from the server closes with 1002") {
    // A continuation frame without a preceding fragment start.
    handler.session->send_raw(WsOpcode::Continuation, true, bytes("x"));
    REQUIRE(run_until(reactor, [&] { return ev.errored; }));
    CHECK(ev.error == NetError::Protocol);
    REQUIRE(run_until(reactor, [&] { return handler.closed == 1; }));
    CHECK(handler.last_close_code == 1002);
  }
  SUBCASE("oversized server message is rejected with 1009") {
    std::string huge(300 * 1024, 'H');  // > 256 KiB client receive buffer
    handler.session->send_text(huge);
    REQUIRE(run_until(reactor, [&] { return ev.errored; }));
    CHECK(ev.error == NetError::Overflow);
    REQUIRE(run_until(reactor, [&] { return handler.closed == 1 || handler.errors == 1; }));
    if (handler.closed == 1) CHECK(handler.last_close_code == 1009);
  }
}

}  // namespace

FASTMM_BACKEND_TEST("ws: echo over PlainStream", test_ws_client_1) {
  Reactor reactor(backend);
  EchoHandler handler;
  PlainServer server(reactor, [](TcpSocket&& s) { return PlainStream(std::move(s)); });
  server.set_ws_handler(&handler);
  REQUIRE(server.listen(SockAddr::loopback_v4(0)));
  REQUIRE(server.port() != 0);
  echo_scenario<PlainClient>(
      reactor, server, handler, [&] { return connect_plain(server.port()); });
}

FASTMM_BACKEND_TEST("ws: echo over TlsStream with the fixture certificate", test_ws_client_2) {
  Reactor reactor(backend);
  EchoHandler handler;
  TlsContext sctx =
      TlsContext::server(tls_fixture("cert.pem").string(), tls_fixture("key.pem").string());
  TlsContext cctx;
  cctx.set_ca_file(tls_fixture("cert.pem").string());
  TlsServer server(reactor, [&](TcpSocket&& s) {
    return TlsStream<PlainStream>(sctx, PlainStream(std::move(s)));
  });
  server.set_ws_handler(&handler);
  REQUIRE(server.listen(SockAddr::loopback_v4(0)));
  echo_scenario<TlsClient>(reactor, server, handler, [&] {
    return TlsStream<PlainStream>(cctx, connect_plain(server.port()), "localhost");
  });
}

FASTMM_BACKEND_TEST("ws: upgrade rejected by the server surfaces as a protocol error",
                    test_ws_client_3) {
  Reactor reactor(backend);
  EchoHandler handler;
  PlainServer server(reactor, [](TcpSocket&& s) { return PlainStream(std::move(s)); });
  server.set_ws_handler(&handler);
  REQUIRE(server.listen(SockAddr::loopback_v4(0)));
  ClientEvents ev;
  PlainClient client(reactor, connect_plain(server.port()), ev);
  REQUIRE(client.start("localhost", server.port(), false, "/forbidden"));
  REQUIRE(run_until(reactor, [&] { return ev.finished(); }));
  CHECK(ev.errored);
  CHECK(ev.error == NetError::Protocol);
  CHECK(ev.error_detail.find("non-101") != std::string::npos);
  CHECK_FALSE(ev.open);
  CHECK(handler.opened == 0);
}

FASTMM_BACKEND_TEST("ws: TLS hostname mismatch fails before the upgrade", test_ws_client_4) {
  Reactor reactor(backend);
  EchoHandler handler;
  TlsContext sctx =
      TlsContext::server(tls_fixture("cert.pem").string(), tls_fixture("key.pem").string());
  TlsContext cctx;
  cctx.set_ca_file(tls_fixture("cert.pem").string());
  TlsServer server(reactor, [&](TcpSocket&& s) {
    return TlsStream<PlainStream>(sctx, PlainStream(std::move(s)));
  });
  server.set_ws_handler(&handler);
  REQUIRE(server.listen(SockAddr::loopback_v4(0)));
  ClientEvents ev;
  TlsClient client(
      reactor, TlsStream<PlainStream>(cctx, connect_plain(server.port()), "wrong.example"), ev);
  REQUIRE(client.start("wrong.example", server.port(), true, "/"));
  REQUIRE(run_until(reactor, [&] { return ev.finished(); }));
  CHECK(ev.error == NetError::Tls);
  CHECK(client.stream().last_error().find("mismatch") != std::string_view::npos);
}

FASTMM_BACKEND_TEST("ws: connection refused is reported as an error", test_ws_client_5) {
  Reactor reactor(backend);
  std::uint16_t port = 0;
  {
    TcpSocket probe = listen_ephemeral(port);  // closed at scope exit: port now refuses
  }
  ClientEvents ev;
  PlainClient client(reactor, connect_plain(port), ev);
  REQUIRE(client.start("localhost", port, false, "/"));
  REQUIRE(run_until(reactor, [&] { return ev.finished(); }));
  CHECK(ev.errored);
  CHECK((ev.error == NetError::Syscall || ev.error == NetError::Closed));
}

FASTMM_BACKEND_TEST("ws: server broadcast reaches every session and reaps closed ones",
                    test_ws_client_6) {
  Reactor reactor(backend);
  EchoHandler handler;
  handler.echo = false;
  PlainServer server(reactor, [](TcpSocket&& s) { return PlainStream(std::move(s)); });
  server.set_ws_handler(&handler);
  REQUIRE(server.listen(SockAddr::loopback_v4(0)));
  ClientEvents ev1;
  ClientEvents ev2;
  PlainClient c1(reactor, connect_plain(server.port()), ev1);
  PlainClient c2(reactor, connect_plain(server.port()), ev2);
  REQUIRE(c1.start("localhost", server.port(), false, "/a"));
  REQUIRE(c2.start("localhost", server.port(), false, "/b"));
  REQUIRE(run_until(reactor, [&] { return ev1.open && ev2.open; }));
  CHECK(server.ws_sessions() == 2);
  CHECK(server.stats().upgrades == 2);
  server.broadcast_text("tick");
  REQUIRE(run_until(reactor, [&] { return ev1.texts.size() == 1 && ev2.texts.size() == 1; }));
  c1.close();  // abrupt, from the client side
  REQUIRE(run_until(reactor, [&] { return server.ws_sessions() == 1; }));
  CHECK(handler.errors == 1);
  server.close_all();
  REQUIRE(run_until(reactor, [&] { return ev2.errored && server.ws_sessions() == 0; }));
}

FASTMM_BACKEND_TEST("ws: send_text reports a write that failed and closed the stream",
                    test_ws_client_7) {
  Reactor reactor(backend);
  EchoHandler handler;
  PlainServer server(reactor, [](TcpSocket&& s) { return PlainStream(std::move(s)); });
  server.set_ws_handler(&handler);
  REQUIRE(server.listen(SockAddr::loopback_v4(0)));
  {
    ClientEvents ev;
    PlainClient client(reactor, connect_plain(server.port()), ev);
    REQUIRE(client.start("localhost", server.port(), false, "/"));
    REQUIRE(run_until(reactor, [&] { return ev.open; }));
    CHECK(client.send_text("ok"));
    // Half-closing the socket makes the next write fail with EPIPE (sends use MSG_NOSIGNAL).
    REQUIRE(::shutdown(client.fd(), SHUT_WR) == 0);
    CHECK_FALSE(client.send_text("never leaves"));
    CHECK_FALSE(client.is_open());
    CHECK(ev.errored);
  }
  {
    // Corked, a data frame is only encoded: uncork() reports the write of the batch.
    ClientEvents ev;
    PlainClient client(reactor, connect_plain(server.port()), ev);
    REQUIRE(client.start("localhost", server.port(), false, "/"));
    REQUIRE(run_until(reactor, [&] { return ev.open; }));
    client.cork();
    REQUIRE(::shutdown(client.fd(), SHUT_WR) == 0);
    CHECK(client.send_text("queued"));
    CHECK_FALSE(client.uncork());
    CHECK_FALSE(client.is_open());
  }
}
