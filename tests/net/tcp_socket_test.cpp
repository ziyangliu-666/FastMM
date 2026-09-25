#include "fastmm/net/tcp_socket.hpp"

#include "net_test_util.hpp"

#include <array>
#include <string>

using namespace fastmm::net;
using namespace fastmm::net::test;

TEST_CASE("SockAddr: parse/format IPv4 and IPv6") {
  auto v4 = SockAddr::from_ip("192.168.1.10", 8080);
  REQUIRE(v4);
  CHECK(v4->family() == AF_INET);
  CHECK(v4->port() == 8080);
  CHECK(v4->to_string() == "192.168.1.10:8080");

  auto v6 = SockAddr::from_ip("::1", 443);
  REQUIRE(v6);
  CHECK(v6->family() == AF_INET6);
  CHECK(v6->port() == 443);
  CHECK(v6->to_string() == "[::1]:443");

  CHECK_FALSE(SockAddr::from_ip("not-an-ip", 1));
  CHECK_FALSE(SockAddr::from_ip("", 1));
  CHECK(SockAddr::loopback_v4(9).to_string() == "127.0.0.1:9");
  CHECK(SockAddr::any_v4(9).to_string() == "0.0.0.0:9");

  char small[4];
  CHECK(v4->format(small) == 0);
  CHECK_FALSE(SockAddr{}.valid());
  CHECK(SockAddr{}.port() == 0);
}

TEST_CASE("TcpSocket: socketpair read/write semantics") {
  TcpSocket a, b;
  REQUIRE(TcpSocket::make_pair(a, b));
  CHECK(a.valid());
  CHECK(b.valid());

  std::byte buf[64];
  auto r = a.read(buf);  // nothing yet
  CHECK(r.want_read);
  CHECK(r.bytes == 0);
  CHECK_FALSE(r.failed());

  REQUIRE(b.write(bytes("abc")).bytes == 3);
  r = a.read(buf);
  CHECK(r.ok());
  CHECK(sv(std::span<const std::byte>(buf, r.bytes)) == "abc");

  SUBCASE("EOF after peer closes") {
    b.close();
    CHECK_FALSE(b.valid());
    r = a.read(buf);
    CHECK(r.closed);
    CHECK(r.error == NetError::Closed);
  }
  SUBCASE("write to a closed peer reports EOF/EPIPE, not SIGPIPE") {
    b.close();
    IoResult w;
    // The first write may succeed into the kernel buffer; keep going until it fails.
    for (int i = 0; i < 100; ++i) {
      w = a.write(bytes("data"));
      if (!w.ok()) break;
    }
    CHECK((w.closed || w.error == NetError::Syscall));
  }
  SUBCASE("partial write when the kernel buffer is full") {
    REQUIRE(a.set_sndbuf(4096));
    std::string big(1 << 20, 'z');
    auto w = a.write(bytes(big));
    CHECK(w.want_write);
    CHECK(w.bytes > 0);
    CHECK(w.bytes < big.size());
  }
  SUBCASE("move transfers ownership") {
    const int fd = a.fd();
    TcpSocket c(std::move(a));
    CHECK(c.fd() == fd);
    CHECK_FALSE(a.valid());  // NOLINT(bugprone-use-after-move)
    TcpSocket d;
    d = std::move(c);
    CHECK(d.fd() == fd);
    CHECK(d.release() == fd);
    CHECK_FALSE(d.valid());
    TcpSocket owner(fd);  // re-adopt so it is closed
  }
  SUBCASE("empty read is a no-op") {
    CHECK(a.read(std::span<std::byte>{}).ok());
    CHECK(a.write(std::span<const std::byte>{}).ok());
  }
}

TEST_CASE("TcpSocket: input_drained reports a short read") {
  TcpSocket a;
  TcpSocket b;
  REQUIRE(TcpSocket::make_pair(a, b));
  CHECK_FALSE(b.input_drained());
  const std::string msg(100, 'x');
  REQUIRE(a.write(bytes(msg)).ok());
  std::array<std::byte, 60> small{};
  IoResult r = b.read(small);
  CHECK(r.bytes == 60);
  CHECK_FALSE(b.input_drained());  // filled the buffer: more may be queued
  std::array<std::byte, 256> big{};
  r = b.read(big);
  CHECK(r.bytes == 40);
  CHECK(b.input_drained());  // short read: the queue was empty after it
  r = b.read(big);
  CHECK(r.want_read);
  CHECK_FALSE(b.input_drained());
}

TEST_CASE("TcpSocket: listen on an ephemeral port and accept") {
  std::uint16_t port = 0;
  TcpSocket listener = listen_ephemeral(port);
  CHECK_FALSE(listener.accept().valid());  // nothing pending -> invalid, no error
  CHECK(listener.last_error() == 0);

  TcpSocket client;
  auto st = client.connect(SockAddr::loopback_v4(port));
  REQUIRE(st != ConnectStatus::Error);
  // Loopback connects finish almost immediately; spin a little for accept.
  TcpSocket server;
  for (int i = 0; i < 1000 && !server.valid(); ++i) server = listener.accept();
  REQUIRE(server.valid());
  if (st == ConnectStatus::InProgress) CHECK(client.finish_connect() == 0);
  CHECK_FALSE(client.connecting());
  CHECK(server.set_nodelay(true));
  CHECK(client.local_addr()->family() == AF_INET);

  REQUIRE(client.write(bytes("hi")).ok());
  std::byte buf[8];
  IoResult r;
  for (int i = 0; i < 1000; ++i) {
    r = server.read(buf);
    if (r.bytes > 0) break;
  }
  CHECK(sv(std::span<const std::byte>(buf, r.bytes)) == "hi");

  SUBCASE("binding the same port twice fails") {
    TcpSocket dup = TcpSocket::listen(SockAddr::loopback_v4(port));
    CHECK_FALSE(dup.valid());
    CHECK(dup.last_error() == EADDRINUSE);
  }
}
