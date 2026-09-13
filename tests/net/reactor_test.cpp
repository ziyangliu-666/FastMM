#include "fastmm/net/reactor.hpp"

#include "net_test_util.hpp"

#include "fastmm/net/tcp_socket.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace fastmm::net;
using namespace fastmm::net::test;

namespace {

// Echoes everything it reads back to the same socket; drains edge-triggered.
class EchoHandler final : public IoHandler {
 public:
  explicit EchoHandler(TcpSocket& s) : sock_(s) {}
  void on_readable() override {
    std::byte buf[256];
    for (;;) {
      auto r = sock_.read(buf);
      if (r.bytes > 0) {
        received += r.bytes;
        sock_.write(std::span<const std::byte>(buf, r.bytes));
      }
      if (r.closed) {
        closed = true;
        return;
      }
      if (r.want_read || r.failed()) return;
    }
  }
  void on_writable() override { ++writable; }
  void on_error(int) override { ++errors; }

  std::size_t received = 0;
  int writable = 0;
  int errors = 0;
  bool closed = false;

 private:
  TcpSocket& sock_;
};

class CollectHandler final : public IoHandler {
 public:
  explicit CollectHandler(TcpSocket& s) : sock_(s) {}
  void on_readable() override {
    std::byte buf[4096];
    for (;;) {
      auto r = sock_.read(buf);
      if (r.bytes > 0) data.append(reinterpret_cast<const char*>(buf), r.bytes);
      if (r.closed) closed = true;
      if (r.bytes == 0) return;
    }
  }
  void on_writable() override {}
  void on_error(int) override {}
  std::string data;
  bool closed = false;

 private:
  TcpSocket& sock_;
};

}  // namespace

TEST_CASE("reactor: timers fire in deadline order and can be cancelled") {
  Reactor r;
  std::vector<int> order;
  const auto now = Reactor::now_ns();
  r.add_timer(now + 30'000'000, [&] { order.push_back(3); });
  r.add_timer(now + 10'000'000, [&] { order.push_back(1); });
  const TimerId t2 = r.add_timer(now + 20'000'000, [&] { order.push_back(2); });
  r.add_timer(now + 20'000'000, [&] { order.push_back(22); });  // same deadline: insertion order
  CHECK(r.active_timers() == 4);
  CHECK(r.cancel_timer(t2));
  CHECK_FALSE(r.cancel_timer(t2));
  CHECK(r.active_timers() == 3);
  REQUIRE(run_until(r, [&] { return order.size() == 3; }));
  CHECK(order == std::vector<int>{1, 22, 3});
  CHECK(r.active_timers() == 0);
}

TEST_CASE("reactor: timer callback may re-arm itself") {
  Reactor r;
  int ticks = 0;
  std::function<void()> tick = [&] {
    if (++ticks < 5) r.add_timer_after(1'000'000, tick);
  };
  r.add_timer_after(1'000'000, tick);
  REQUIRE(run_until(r, [&] { return ticks == 5; }));
}

TEST_CASE("reactor: run_once timeout is bounded by the next timer") {
  Reactor r;
  bool fired = false;
  r.add_timer_after(20'000'000, [&] { fired = true; });
  const auto t0 = std::chrono::steady_clock::now();
  // Each call would block 5 s without the timer bound.
  for (int i = 0; i < 3 && !fired; ++i) r.run_once(5000);
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  CHECK(fired);
  CHECK(elapsed < std::chrono::milliseconds(1000));
}

TEST_CASE("reactor: wake() and post() from another thread") {
  Reactor r;
  std::atomic<bool> posted{false};
  std::thread t([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    r.post([&] { posted = true; });
  });
  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE(run_until(r, [&] { return posted.load(); }, 3000));
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
  t.join();

  SUBCASE("wake interrupts a long blocking wait") {
    std::thread w([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      r.wake();
    });
    const auto t1 = std::chrono::steady_clock::now();
    r.run_once(5000);
    CHECK(std::chrono::steady_clock::now() - t1 < std::chrono::seconds(1));
    w.join();
  }
  SUBCASE("run(stop) exits when the flag is set") {
    std::atomic<bool> stop{false};
    std::thread s([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      stop = true;
      r.wake();
    });
    r.run(stop);
    s.join();
    CHECK(stop);
  }
}

TEST_CASE("reactor: busy poll mode returns immediately") {
  Reactor r;
  r.set_busy_poll(true);
  CHECK(r.busy_poll());
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 100; ++i) r.run_once(1000);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
}

TEST_CASE("reactor: socketpair echo through edge-triggered handlers") {
  Reactor r;
  TcpSocket a;
  TcpSocket b;
  REQUIRE(TcpSocket::make_pair(a, b));
  EchoHandler echo(b);
  CollectHandler collect(a);
  REQUIRE(r.add(b.fd(), echo, IoEvent::Read));
  REQUIRE(r.add(a.fd(), collect, IoEvent::Read));
  CHECK(r.is_registered(a.fd()));
  CHECK_FALSE(r.add(a.fd(), collect, IoEvent::Read));  // duplicate add fails (EEXIST)

  const std::string msg = "ping-pong over AF_UNIX";
  auto w = a.write(bytes(msg));
  REQUIRE(w.ok());
  CHECK(w.bytes == msg.size());
  REQUIRE(run_until(r, [&] { return collect.data.size() == msg.size(); }));
  CHECK(collect.data == msg);
  CHECK(echo.received == msg.size());

  SUBCASE("EOF surfaces as a readable event") {
    a.shutdown_write();
    REQUIRE(run_until(r, [&] { return echo.closed; }));
  }
  SUBCASE("remove stops delivery") {
    CHECK(r.remove(a.fd()));
    CHECK_FALSE(r.remove(a.fd()));
    CHECK_FALSE(r.is_registered(a.fd()));
    a.write(bytes("x"));
    for (int i = 0; i < 5; ++i) r.run_once(5);
    CHECK(collect.data == msg);  // echo happened but nobody collected it
  }
  SUBCASE("modify to write-interest delivers on_writable") {
    REQUIRE(r.modify(b.fd(), echo, IoEvent::ReadWrite));
    REQUIRE(run_until(r, [&] { return echo.writable > 0; }));
  }
}

TEST_CASE("reactor: loopback listen/accept/connect over an ephemeral port") {
  Reactor r;
  std::uint16_t port = 0;
  TcpSocket listener = listen_ephemeral(port);

  class Acceptor final : public IoHandler {
   public:
    Acceptor(TcpSocket& l, Reactor& re) : listener_(l), reactor_(re) {}
    void on_readable() override {
      for (;;) {
        SockAddr peer;
        TcpSocket s = listener_.accept(&peer);
        if (!s.valid()) return;
        CHECK(peer.family() == AF_INET);
        accepted.push_back(std::move(s));
        auto& sock = accepted.back();
        echo = std::make_unique<EchoHandler>(sock);
        reactor_.add(sock.fd(), *echo, IoEvent::Read);
      }
    }
    void on_writable() override {}
    void on_error(int) override {}
    std::vector<TcpSocket> accepted;
    std::unique_ptr<EchoHandler> echo;

   private:
    TcpSocket& listener_;
    Reactor& reactor_;
  } acceptor(listener, r);
  acceptor.accepted.reserve(4);
  REQUIRE(r.add(listener.fd(), acceptor, IoEvent::Read));

  TcpSocket client;
  const auto st = client.connect(SockAddr::loopback_v4(port));
  REQUIRE(st != ConnectStatus::Error);

  class ConnectHandler final : public IoHandler {
   public:
    explicit ConnectHandler(TcpSocket& s) : sock_(s) {}
    void on_readable() override {
      std::byte buf[128];
      auto res = sock_.read(buf);
      if (res.bytes > 0) data.append(reinterpret_cast<const char*>(buf), res.bytes);
    }
    void on_writable() override {
      if (sock_.connecting()) connect_err = sock_.finish_connect();
      connected = true;
    }
    void on_error(int e) override { err = e; }
    bool connected = false;
    int connect_err = -1;
    int err = 0;
    std::string data;

   private:
    TcpSocket& sock_;
  } ch(client);
  REQUIRE(r.add(client.fd(), ch, IoEvent::ReadWrite));
  REQUIRE(run_until(r, [&] { return ch.connected; }));
  if (st == ConnectStatus::InProgress) CHECK(ch.connect_err == 0);
  REQUIRE(client.set_nodelay(true));
  REQUIRE(client.set_keepalive(true));
  REQUIRE(client.set_rcvbuf(1 << 20));
  CHECK(client.peer_addr()->port() == port);

  REQUIRE(client.write(bytes("hello")).ok());
  REQUIRE(run_until(r, [&] { return ch.data == "hello"; }));
  CHECK(acceptor.accepted.size() == 1);

  SUBCASE("connect to a closed port fails") {
    TcpSocket other;
    listener.close();
    r.remove(listener.fd());
    const auto st2 = other.connect(SockAddr::loopback_v4(port));
    if (st2 == ConnectStatus::InProgress) {
      ConnectHandler h2(other);
      REQUIRE(r.add(other.fd(), h2, IoEvent::ReadWrite));
      REQUIRE(run_until(r, [&] { return h2.connected || h2.err != 0; }));
      CHECK((h2.connect_err != 0 || h2.err != 0));
    } else {
      CHECK(st2 == ConnectStatus::Error);
    }
  }
}

TEST_CASE("reactor: handler removed during a batch is not dispatched") {
  // Two sockets become readable in the same epoll batch; whichever handler runs first
  // removes the other's fd. The other must then not be dispatched (its registration is
  // gone), so exactly one handler runs.
  Reactor r;
  TcpSocket a1, b1, a2, b2;
  REQUIRE(TcpSocket::make_pair(a1, b1));
  REQUIRE(TcpSocket::make_pair(a2, b2));
  class Mutual final : public IoHandler {
   public:
    Mutual(Reactor& re, int& calls) : r_(re), calls_(calls) {}
    void on_readable() override {
      ++calls_;
      r_.remove(other_fd);
    }
    void on_writable() override {}
    void on_error(int) override {}
    int other_fd = -1;

   private:
    Reactor& r_;
    int& calls_;
  };
  int calls = 0;
  Mutual first(r, calls);
  Mutual second(r, calls);
  first.other_fd = b2.fd();
  second.other_fd = b1.fd();
  REQUIRE(r.add(b1.fd(), first, IoEvent::Read));
  REQUIRE(r.add(b2.fd(), second, IoEvent::Read));
  a1.write(bytes("x"));
  a2.write(bytes("y"));
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  r.run_once(100);
  CHECK(calls == 1);
}

TEST_CASE("reactor: now_ns is monotonic") {
  const auto a = Reactor::now_ns();
  const auto b = Reactor::now_ns();
  CHECK(b >= a);
  CHECK(a > 0);
}
