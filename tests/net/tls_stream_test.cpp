#include "fastmm/net/tls_stream.hpp"

#include "net_test_util.hpp"

#include "fastmm/net/memory_transport.hpp"
#include "fastmm/net/reactor.hpp"

#include <memory>
#include <string>

using namespace fastmm::net;
using namespace fastmm::net::test;

namespace {

using MemTls = TlsStream<MemoryTransport>;

TlsContext client_ctx_trusting_fixture() {
  TlsContext ctx;
  ctx.set_ca_file(tls_fixture("cert.pem").string());
  return ctx;
}

TlsContext server_ctx() {
  return TlsContext::server(tls_fixture("cert.pem").string(), tls_fixture("key.pem").string());
}

// Alternates client/server handshake steps until both complete or one fails.
struct HandshakeOutcome {
  IoResult client;
  IoResult server;
  bool both_ok() const {
    return client.ok() && !client.would_block() && server.ok() && !server.would_block();
  }
};

HandshakeOutcome pump_handshake(MemTls& client, MemTls& server) {
  HandshakeOutcome o;
  for (int i = 0; i < 64; ++i) {
    o.client = client.handshake();
    o.server = server.handshake();
    if (o.client.failed() || o.server.failed()) return o;
    if (o.both_ok()) return o;
  }
  return o;
}

// Sends `payload` from `from` to `to`, draining the reader as needed; returns received bytes.
std::string transfer(MemTls& from, MemTls& to, const std::string& payload) {
  std::string received;
  std::size_t sent = 0;
  std::string chunk(8192, '\0');
  int guard = 0;
  while (received.size() < payload.size()) {
    REQUIRE(++guard < 200000);
    if (sent < payload.size()) {
      auto w = from.write(bytes(std::string_view(payload).substr(sent)));
      REQUIRE_FALSE(w.failed());
      sent += w.bytes;
    } else {
      REQUIRE_FALSE(from.flush().failed());
    }
    for (;;) {
      auto r = to.read(mutable_bytes(chunk));
      REQUIRE_FALSE(r.failed());
      if (r.bytes > 0) received.append(chunk, 0, r.bytes);
      if (r.would_block() || r.bytes == 0) break;
    }
  }
  return received;
}

}  // namespace

TEST_CASE("tls: in-memory handshake against the fixture CA") {
  MemoryLink link;
  TlsContext cctx = client_ctx_trusting_fixture();
  TlsContext sctx = server_ctx();
  MemTls client(cctx, link.end_a(), "localhost");
  MemTls server(sctx, link.end_b());

  CHECK_FALSE(client.handshake_done());
  auto o = pump_handshake(client, server);
  CAPTURE(client.last_error());
  CAPTURE(server.last_error());
  REQUIRE(o.both_ok());
  CHECK(client.handshake_done());
  CHECK(server.handshake_done());
  CHECK(client.engine().protocol_version() == "TLSv1.3");
  CHECK_FALSE(client.engine().cipher_name().empty());

  SUBCASE("small round trip") {
    CHECK(transfer(client, server, "hello over tls") == "hello over tls");
    CHECK(transfer(server, client, "and back") == "and back");
  }
  SUBCASE("bulk 4 MiB both directions with partial reads and writes") {
    std::string big(4 * 1024 * 1024, '\0');
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31 + (i >> 8));
    link.a_to_b.capacity = 40000;  // smaller than one BIO buffer -> SSL sees want_write
    link.a_to_b.max_read_chunk = 777;
    link.b_to_a.capacity = 12345;
    link.b_to_a.max_write_chunk = 999;
    CHECK(transfer(client, server, big) == big);
    CHECK(transfer(server, client, big) == big);
  }
  SUBCASE("IP literal host verifies against the iPAddress SAN") {
    MemoryLink link2;
    TlsContext c2 = client_ctx_trusting_fixture();
    TlsContext s2 = server_ctx();
    MemTls ipclient(c2, link2.end_a(), "127.0.0.1");
    MemTls ipserver(s2, link2.end_b());
    auto o2 = pump_handshake(ipclient, ipserver);
    CAPTURE(ipclient.last_error());
    CHECK(o2.both_ok());
  }
  SUBCASE("orderly shutdown") {
    IoResult c;
    IoResult s;
    for (int i = 0; i < 16; ++i) {
      c = client.shutdown();
      s = server.shutdown();
      if (!c.would_block() && !s.would_block()) break;
    }
    CHECK(c.ok());
    CHECK(s.ok());
  }
}

TEST_CASE("tls: hostname mismatch fails verification") {
  MemoryLink link;
  TlsContext cctx = client_ctx_trusting_fixture();
  TlsContext sctx = server_ctx();
  MemTls client(cctx, link.end_a(), "not-localhost.example");
  MemTls server(sctx, link.end_b());
  auto o = pump_handshake(client, server);
  CHECK(o.client.failed());
  CHECK(o.client.error == NetError::Tls);
  CHECK(client.last_error().find("verify") != std::string_view::npos);
  CHECK(client.last_error().find("mismatch") != std::string_view::npos);
}

TEST_CASE("tls: untrusted self-signed certificate fails with the system store") {
  MemoryLink link;
  TlsContext cctx;  // system CAs only
  TlsContext sctx = server_ctx();
  MemTls client(cctx, link.end_a(), "localhost");
  MemTls server(sctx, link.end_b());
  auto o = pump_handshake(client, server);
  CHECK(o.client.failed());
  CHECK(client.last_error().find("self-signed") != std::string_view::npos);
}

TEST_CASE("tls: insecure mode skips verification") {
  MemoryLink link;
  TlsContext cctx;
  cctx.set_insecure(true);
  CHECK(cctx.insecure());
  TlsContext sctx = server_ctx();
  MemTls client(cctx, link.end_a(), "whatever.example");
  MemTls server(sctx, link.end_b());
  auto o = pump_handshake(client, server);
  CAPTURE(client.last_error());
  REQUIRE(o.both_ok());
  CHECK(transfer(client, server, "insecure but encrypted") == "insecure but encrypted");
}

TEST_CASE("tls: peer vanishing mid-handshake is an error, after data it is EOF") {
  MemoryLink link;
  TlsContext cctx = client_ctx_trusting_fixture();
  TlsContext sctx = server_ctx();
  MemTls client(cctx, link.end_a(), "localhost");
  MemTls server(sctx, link.end_b());
  REQUIRE(pump_handshake(client, server).both_ok());
  server.close();  // transport EOF without close_notify
  std::string buf(64, '\0');
  IoResult r;
  for (int i = 0; i < 4; ++i) {
    r = client.read(mutable_bytes(buf));
    if (!r.would_block()) break;
  }
  CHECK((r.closed || r.failed()));
}

TEST_CASE("tls: context errors throw during setup only") {
  CHECK_THROWS_AS(TlsContext::server("/nonexistent/cert.pem", "/nonexistent/key.pem"),
                  std::runtime_error);
  TlsContext c;
  CHECK_THROWS_AS(c.set_ca_file("/nonexistent/ca.pem"), std::runtime_error);
  CHECK(c.mode() == TlsContext::Mode::Client);
  CHECK(server_ctx().mode() == TlsContext::Mode::Server);
}

// ---------------------------------------------------------------------- reactor + sockets

namespace {

using SockTls = TlsStream<PlainStream>;

// Drives handshake then echoes (server) or collects (client) over a reactor.
class TlsParty final : public IoHandler {
 public:
  TlsParty(Reactor& r, std::unique_ptr<SockTls> s, bool echo)
      : reactor_(r), stream_(std::move(s)), echo_(echo) {
    reactor_.add(stream_->fd(), *this, IoEvent::ReadWrite);
  }
  ~TlsParty() override { reactor_.remove(stream_->fd()); }

  void on_readable() override { step(); }
  void on_writable() override { step(); }
  void on_error(int e) override { error = e; }

  // Queues bytes; partial writes are resumed from on_writable().
  void send(std::string_view s) {
    out_.append(s);
    step();
  }

  bool ready = false;
  bool closed = false;
  int error = 0;
  std::string received;
  IoResult last_handshake;

 private:
  void step() {
    if (!ready) {
      last_handshake = stream_->handshake();
      if (last_handshake.failed() || last_handshake.would_block()) return;
      ready = true;
    }
    stream_->flush();
    if (out_off_ < out_.size()) {
      auto w = stream_->write(bytes(std::string_view(out_).substr(out_off_)));
      REQUIRE_FALSE(w.failed());
      out_off_ += w.bytes;
    }
    std::byte buf[4096];
    for (;;) {
      auto r = stream_->read(buf);
      if (r.bytes > 0) {
        received.append(reinterpret_cast<const char*>(buf), r.bytes);
        if (echo_) stream_->write(std::span<const std::byte>(buf, r.bytes));
      }
      if (r.closed) {
        closed = true;
        return;
      }
      if (r.would_block() || r.failed()) return;
    }
  }

  Reactor& reactor_;
  std::unique_ptr<SockTls> stream_;
  bool echo_;
  std::string out_;
  std::size_t out_off_ = 0;
};

}  // namespace

FASTMM_BACKEND_TEST("tls: loopback sockets driven by the reactor", test_tls_stream_1) {
  Reactor reactor(backend);
  std::uint16_t port = 0;
  TcpSocket listener = listen_ephemeral(port);
  TlsContext sctx = server_ctx();
  TlsContext cctx = client_ctx_trusting_fixture();

  std::unique_ptr<TlsParty> server;
  class Acceptor final : public IoHandler {
   public:
    Acceptor(Reactor& r, TcpSocket& l, TlsContext& c, std::unique_ptr<TlsParty>& out)
        : r_(r), l_(l), c_(c), out_(out) {}
    void on_readable() override {
      TcpSocket s = l_.accept();
      if (!s.valid()) return;
      out_ = std::make_unique<TlsParty>(
          r_, std::make_unique<SockTls>(c_, PlainStream(std::move(s))), true);
    }
    void on_writable() override {}
    void on_error(int) override {}

   private:
    Reactor& r_;
    TcpSocket& l_;
    TlsContext& c_;
    std::unique_ptr<TlsParty>& out_;
  } acceptor(reactor, listener, sctx, server);
  REQUIRE(reactor.add(listener.fd(), acceptor, IoEvent::Read));

  TcpSocket csock;
  REQUIRE(csock.connect(SockAddr::loopback_v4(port)) != ConnectStatus::Error);
  TlsParty client(
      reactor, std::make_unique<SockTls>(cctx, PlainStream(std::move(csock)), "localhost"), false);

  REQUIRE(run_until(reactor, [&] { return client.ready && server && server->ready; }));
  const std::string msg(200000, 'q');
  client.send(msg);
  REQUIRE(run_until(reactor, [&] { return client.received.size() == msg.size(); }));
  CHECK(client.received == msg);
  CHECK(client.error == 0);
}
