#include "fastmm/net/reactor.hpp"

#include "net_test_util.hpp"

#include "fastmm/net/tcp_socket.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
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
    ++readable;
    std::byte buf[4096];
    bool got_any = false;
    for (;;) {
      auto r = sock_.read(buf);
      if (r.bytes > 0) {
        got_any = true;
        data.append(reinterpret_cast<const char*>(buf), r.bytes);
      }
      if (r.closed) {
        closed = true;
        got_any = true;
      }
      if (r.bytes == 0) break;
    }
    if (!got_any) ++empty_reads;
  }
  void on_writable() override { ++writable; }
  void on_error(int) override { ++errors; }
  std::string data;
  bool closed = false;
  int readable = 0;
  int empty_reads = 0;  // on_readable() with nothing to read
  int writable = 0;
  int errors = 0;

 private:
  TcpSocket& sock_;
};

// Reads `s` directly (not through a reactor) until EOF or `timeout_ms`.
bool peer_sees_eof(TcpSocket& s, int timeout_ms = 2000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  std::byte buf[256];
  while (std::chrono::steady_clock::now() < deadline) {
    auto r = s.read(buf);
    if (r.closed) return true;
    if (r.bytes == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

}  // namespace

TEST_CASE("reactor: backend names and selection") {
  ReactorBackend b = ReactorBackend::IoUring;
  CHECK(parse_reactor_backend("epoll", b));
  CHECK(b == ReactorBackend::Epoll);
  CHECK(parse_reactor_backend("io_uring", b));
  CHECK(b == ReactorBackend::IoUring);
  CHECK_FALSE(parse_reactor_backend("uring", b));
  CHECK(b == ReactorBackend::IoUring);
  CHECK(to_string(ReactorBackend::Epoll) == "epoll");
  CHECK(to_string(ReactorBackend::IoUring) == "io_uring");
  CHECK(Reactor::resolve_backend(ReactorBackend::Epoll) == ReactorBackend::Epoll);
  CHECK(Reactor::resolve_backend(ReactorBackend::IoUring) ==
        (Reactor::io_uring_supported() ? ReactorBackend::IoUring : ReactorBackend::Epoll));
  Reactor def;
  CHECK(def.backend() == ReactorBackend::Epoll);
  CHECK(def.epoll_fd() >= 0);
  MESSAGE("io_uring supported: " << Reactor::io_uring_supported());
}

FASTMM_BACKEND_TEST("reactor: constructs the requested backend", test_backend_accessor) {
  Reactor r(backend);
  CHECK(r.backend() == backend);
  CHECK((r.epoll_fd() >= 0) == (backend == ReactorBackend::Epoll));
  CHECK(r.run_once(0) >= 0);
}

FASTMM_BACKEND_TEST("reactor: timers fire in deadline order and can be cancelled",
                    test_timer_order) {
  Reactor r(backend);
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

TEST_CASE("reactor: a stale timer id does not cancel the timer that reuses its slot") {
  Reactor r;
  bool first = false;
  bool second = false;
  const TimerId a = r.add_timer_after(1'000'000, [&] { first = true; });
  REQUIRE(r.cancel_timer(a));
  const TimerId b = r.add_timer_after(1'000'000, [&] { second = true; });
  CHECK(a != b);
  CHECK_FALSE(r.cancel_timer(a));
  CHECK_FALSE(r.cancel_timer(kInvalidTimer));
  CHECK(r.active_timers() == 1);
  REQUIRE(run_until(r, [&] { return second; }));
  CHECK_FALSE(first);
  CHECK_FALSE(r.cancel_timer(b));  // fired
  CHECK(r.active_timers() == 0);
}

TEST_CASE("reactor: cancelling and re-arming long timers keeps deadline order") {
  Reactor r;
  std::vector<int> order;
  const auto now = Reactor::now_ns();
  TimerId pending = kInvalidTimer;
  for (int i = 0; i < 1000; ++i) {  // compacts the cancelled entries several times
    r.cancel_timer(pending);
    pending = r.add_timer(now + 3'600'000'000'000, [&] { order.push_back(-1); });
  }
  r.add_timer(now + 20'000'000, [&] { order.push_back(2); });
  r.add_timer(now + 10'000'000, [&] { order.push_back(1); });
  r.add_timer(now + 20'000'000, [&] { order.push_back(3); });  // same deadline: insertion order
  CHECK(r.active_timers() == 4);
  REQUIRE(run_until(r, [&] { return order.size() == 3; }));
  CHECK(order == std::vector<int>{1, 2, 3});
  CHECK(r.active_timers() == 1);
  CHECK(r.cancel_timer(pending));
}

FASTMM_BACKEND_TEST("reactor: timer callback may re-arm itself", test_timer_rearm) {
  Reactor r(backend);
  int ticks = 0;
  std::function<void()> tick = [&] {
    if (++ticks < 5) r.add_timer_after(1'000'000, tick);
  };
  r.add_timer_after(1'000'000, tick);
  REQUIRE(run_until(r, [&] { return ticks == 5; }));
}

FASTMM_BACKEND_TEST("reactor: run_once timeout is bounded by the next timer", test_timer_bound) {
  Reactor r(backend);
  bool fired = false;
  r.add_timer_after(20'000'000, [&] { fired = true; });
  const auto t0 = std::chrono::steady_clock::now();
  // Each call would block 5 s without the timer bound.
  for (int i = 0; i < 3 && !fired; ++i) r.run_once(5000);
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  CHECK(fired);
  CHECK(elapsed < std::chrono::milliseconds(1000));
}

FASTMM_BACKEND_TEST("reactor: an infinite wait wakes for a timer and never fires it early",
                    test_timer_infinite_wait) {
  Reactor r(backend);
  std::int64_t deadline = 0;
  std::int64_t fired_at = 0;
  deadline = Reactor::now_ns() + 15'000'000;
  r.add_timer(deadline, [&] { fired_at = Reactor::now_ns(); });
  const auto t0 = std::chrono::steady_clock::now();
  int calls = 0;
  while (fired_at == 0 && calls < 100) {
    r.run_once(-1);
    ++calls;
  }
  CHECK(fired_at >= deadline);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
  CHECK(calls < 100);
}

FASTMM_BACKEND_TEST("reactor: wake() and post() from another thread", test_wake_post) {
  Reactor r(backend);
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
  SUBCASE("wake interrupts an infinite wait") {
    std::thread w([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      r.wake();
    });
    const auto t1 = std::chrono::steady_clock::now();
    r.run_once(-1);
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

FASTMM_BACKEND_TEST("reactor: posts from several threads all run on the reactor thread",
                    test_many_posts) {
  Reactor r(backend);
  constexpr int kThreads = 4;
  constexpr int kPerThread = 2000;
  int ran = 0;  // only touched on the reactor thread
  const auto reactor_thread = std::this_thread::get_id();
  std::atomic<int> wrong_thread{0};
  std::vector<std::thread> producers;
  producers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    producers.emplace_back([&] {
      for (int i = 0; i < kPerThread; ++i) {
        r.post([&] {
          if (std::this_thread::get_id() != reactor_thread) ++wrong_thread;
          ++ran;
        });
        if (i % 256 == 0) std::this_thread::yield();
      }
    });
  }
  const bool done = run_until(r, [&] { return ran == kThreads * kPerThread; }, 10000);
  for (auto& p : producers) p.join();
  r.run_once(0);
  CHECK(done);
  CHECK(ran == kThreads * kPerThread);
  CHECK(wrong_thread == 0);
}

FASTMM_BACKEND_TEST("reactor: busy poll mode returns immediately", test_busy_poll) {
  Reactor r(backend);
  r.set_busy_poll(true);
  CHECK(r.busy_poll());
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 100; ++i) r.run_once(1000);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));

  // I/O is still delivered without ever blocking.
  TcpSocket a;
  TcpSocket b;
  REQUIRE(TcpSocket::make_pair(a, b));
  EchoHandler echo(b);
  CollectHandler collect(a);
  REQUIRE(r.add(b.fd(), echo, IoEvent::Read));
  REQUIRE(r.add(a.fd(), collect, IoEvent::Read));
  REQUIRE(a.write(bytes("busy")).ok());
  REQUIRE(run_until(r, [&] { return collect.data == "busy"; }));
  r.remove(a.fd());
  r.remove(b.fd());
}

FASTMM_BACKEND_TEST("reactor: socketpair echo through edge-triggered handlers", test_echo) {
  Reactor r(backend);
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
  SUBCASE("modify of an unregistered fd fails") {
    CHECK(r.remove(b.fd()));
    CHECK_FALSE(r.modify(b.fd(), echo, IoEvent::ReadWrite));
  }
}

FASTMM_BACKEND_TEST("reactor: an idle writable socket does not spin", test_idle_writable) {
  Reactor r(backend);
  TcpSocket a;
  TcpSocket b;
  REQUIRE(TcpSocket::make_pair(a, b));
  CollectHandler h(a);
  REQUIRE(r.add(a.fd(), h, IoEvent::ReadWrite));
  REQUIRE(run_until(r, [&] { return h.writable > 0; }));
  int events = 0;
  for (int i = 0; i < 20; ++i) events += r.run_once(2);
  CHECK(h.writable <= 2);
  CHECK(events <= 1);

  SUBCASE("dropping write interest stops on_writable, asking again delivers it") {
    REQUIRE(r.modify(a.fd(), h, IoEvent::Read));
    for (int i = 0; i < 5; ++i) r.run_once(2);
    const int before = h.writable;
    REQUIRE(b.write(bytes("data")).ok());  // wakes the fd for reading only
    REQUIRE(run_until(r, [&] { return h.data == "data"; }));
    CHECK(h.writable == before);
    REQUIRE(r.modify(a.fd(), h, IoEvent::ReadWrite));
    REQUIRE(run_until(r, [&] { return h.writable > before; }));
  }
  r.remove(a.fd());
}

FASTMM_BACKEND_TEST("reactor: loopback listen/accept/connect over an ephemeral port",
                    test_loopback) {
  Reactor r(backend);
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
    // remove() before close(): with io_uring the poll request would keep the listener open.
    r.remove(listener.fd());
    listener.close();
    const auto st2 = other.connect(SockAddr::loopback_v4(port));
    if (st2 == ConnectStatus::InProgress) {
      ConnectHandler h2(other);
      REQUIRE(r.add(other.fd(), h2, IoEvent::ReadWrite));
      REQUIRE(run_until(r, [&] { return h2.connected || h2.err != 0; }));
      CHECK((h2.connect_err != 0 || h2.err != 0));
      r.remove(other.fd());
    } else {
      CHECK(st2 == ConnectStatus::Error);
    }
  }
  if (acceptor.echo) r.remove(acceptor.accepted.back().fd());
  r.remove(client.fd());
}

FASTMM_BACKEND_TEST("reactor: handler removed during a batch is not dispatched",
                    test_remove_in_batch) {
  // Two sockets become readable in the same batch; whichever handler runs first removes the
  // other's fd. The other must then not be dispatched (its registration is gone), so exactly
  // one handler runs.
  Reactor r(backend);
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
  r.run_once(0);  // io_uring: submit the poll requests before the data arrives
  a1.write(bytes("x"));
  a2.write(bytes("y"));
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  r.run_once(100);
  CHECK(calls == 1);
  for (int i = 0; i < 5; ++i) r.run_once(2);
  CHECK(calls == 1);  // the removed registration stays silent
}

FASTMM_BACKEND_TEST("reactor: handler replaced during a batch gets the pending event",
                    test_modify_in_batch) {
  // Whichever handler runs first re-registers the other fd with a third handler; the other
  // fd's event in the same batch must go to the replacement.
  Reactor r(backend);
  TcpSocket a1, b1, a2, b2;
  REQUIRE(TcpSocket::make_pair(a1, b1));
  REQUIRE(TcpSocket::make_pair(a2, b2));
  class Counting final : public IoHandler {
   public:
    explicit Counting(TcpSocket& s) : sock_(s) {}
    void on_readable() override {
      ++calls;
      std::byte buf[64];
      while (sock_.read(buf).bytes > 0) {
      }
    }
    void on_writable() override {}
    void on_error(int) override {}
    int calls = 0;

   private:
    TcpSocket& sock_;
  };
  class Swapper final : public IoHandler {
   public:
    Swapper(Reactor& re, int& calls, TcpSocket& self) : r_(re), calls_(calls), self_(self) {}
    void on_readable() override {
      ++calls_;
      std::byte buf[64];
      while (self_.read(buf).bytes > 0) {
      }
      r_.modify(other_fd, *replacement, IoEvent::Read);
    }
    void on_writable() override {}
    void on_error(int) override {}
    int other_fd = -1;
    IoHandler* replacement = nullptr;

   private:
    Reactor& r_;
    int& calls_;
    TcpSocket& self_;
  };
  int calls = 0;
  Swapper first(r, calls, b1);
  Swapper second(r, calls, b2);
  Counting for_b1(b1);
  Counting for_b2(b2);
  first.other_fd = b2.fd();
  first.replacement = &for_b2;
  second.other_fd = b1.fd();
  second.replacement = &for_b1;
  REQUIRE(r.add(b1.fd(), first, IoEvent::Read));
  REQUIRE(r.add(b2.fd(), second, IoEvent::Read));
  r.run_once(0);
  a1.write(bytes("x"));
  a2.write(bytes("y"));
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  r.run_once(100);
  CHECK(calls == 1);
  CHECK(for_b1.calls + for_b2.calls == 1);
  r.remove(b1.fd());
  r.remove(b2.fd());
}

FASTMM_BACKEND_TEST("reactor: remove then close releases the socket immediately",
                    test_remove_releases) {
  Reactor r(backend);
  TcpSocket a;
  TcpSocket b;
  REQUIRE(TcpSocket::make_pair(a, b));
  CollectHandler h(b);
  REQUIRE(r.add(b.fd(), h, IoEvent::ReadWrite));
  REQUIRE(run_until(r, [&] { return h.writable > 0; }));
  REQUIRE(r.remove(b.fd()));
  b.close();
  CHECK(peer_sees_eof(a));  // without running the reactor again
}

FASTMM_BACKEND_TEST(
    "reactor: completions of an earlier registration of a reused fd number are ignored",
    test_generation) {
  Reactor r(backend);
  TcpSocket a;
  TcpSocket b;
  REQUIRE(TcpSocket::make_pair(a, b));
  CollectHandler old_handler(b);
  REQUIRE(r.add(b.fd(), old_handler, IoEvent::Read));
  r.run_once(0);
  // Readiness for the old registration is reported (a completion is queued) but not dispatched.
  REQUIRE(a.write(bytes("old")).ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  const int number = b.fd();
  REQUIRE(r.remove(number));
  b.close();

  TcpSocket c;
  TcpSocket d;
  REQUIRE(TcpSocket::make_pair(c, d));
  TcpSocket* reused = c.fd() == number ? &c : (d.fd() == number ? &d : nullptr);
  TcpSocket* peer = reused == &c ? &d : &c;
  if (reused == nullptr) {
    MESSAGE("the kernel did not reuse fd " << number << ", nothing to check");
    return;
  }
  CollectHandler new_handler(*reused);
  REQUIRE(r.add(reused->fd(), new_handler, IoEvent::Read));
  for (int i = 0; i < 5; ++i) r.run_once(2);
  CHECK(old_handler.readable == 0);
  CHECK(new_handler.readable == 0);  // the stale "old" readiness went nowhere
  REQUIRE(peer->write(bytes("new")).ok());
  REQUIRE(run_until(r, [&] { return new_handler.data == "new"; }));
  CHECK(new_handler.empty_reads == 0);
  CHECK(old_handler.readable == 0);
  r.remove(reused->fd());
}

FASTMM_BACKEND_TEST("reactor: a closed fd whose number is reused can be registered again",
                    test_close_without_remove) {
  Reactor r(backend);
  TcpSocket a;
  TcpSocket b;
  REQUIRE(TcpSocket::make_pair(a, b));
  CollectHandler old_handler(b);
  REQUIRE(r.add(b.fd(), old_handler, IoEvent::Read));
  r.run_once(0);
  const int number = b.fd();
  b.close();  // no remove(): the registration is stale

  TcpSocket c;
  TcpSocket d;
  REQUIRE(TcpSocket::make_pair(c, d));
  TcpSocket* reused = c.fd() == number ? &c : (d.fd() == number ? &d : nullptr);
  TcpSocket* peer = reused == &c ? &d : &c;
  if (reused == nullptr) {
    MESSAGE("the kernel did not reuse fd " << number << ", nothing to check");
    return;
  }
  CollectHandler new_handler(*reused);
  REQUIRE(r.add(reused->fd(), new_handler, IoEvent::Read));
  REQUIRE(peer->write(bytes("fresh")).ok());
  REQUIRE(run_until(r, [&] { return new_handler.data == "fresh"; }));
  CHECK(old_handler.readable == 0);
  // The old socket is really gone once the stale registration has been cancelled.
  bool eof = false;
  REQUIRE(run_until(r, [&] {
    std::byte buf[16];
    eof = eof || a.read(buf).closed;
    return eof;
  }));
  r.remove(reused->fd());
}

FASTMM_BACKEND_TEST("reactor: add rejects invalid descriptors and regular files",
                    test_add_rejects) {
  Reactor r(backend);
  TcpSocket a;
  TcpSocket b;
  REQUIRE(TcpSocket::make_pair(a, b));
  CollectHandler h(a);
  CHECK_FALSE(r.add(-1, h, IoEvent::Read));
  const int closed_fd = b.fd();
  b.close();
  CHECK_FALSE(r.add(closed_fd, h, IoEvent::Read));
  CHECK_FALSE(r.is_registered(closed_fd));
  std::FILE* file = std::tmpfile();
  REQUIRE(file != nullptr);
  CHECK_FALSE(r.add(::fileno(file), h, IoEvent::Read));
  std::fclose(file);
  CHECK_FALSE(r.remove(closed_fd));
  CHECK_FALSE(r.modify(closed_fd, h, IoEvent::Read));
}

FASTMM_BACKEND_TEST("reactor: hundreds of registered fds", test_many_fds) {
  // More registrations than the io_uring submission queue holds (512) before the first run.
  Reactor r(backend);
  constexpr std::size_t kPairs = 300;
  std::vector<TcpSocket> left(kPairs);
  std::vector<TcpSocket> right(kPairs);
  std::vector<std::unique_ptr<EchoHandler>> echoes;
  std::vector<std::unique_ptr<CollectHandler>> collectors;
  echoes.reserve(kPairs);
  collectors.reserve(kPairs);
  for (std::size_t i = 0; i < kPairs; ++i) {
    REQUIRE(TcpSocket::make_pair(left[i], right[i]));
    echoes.push_back(std::make_unique<EchoHandler>(right[i]));
    collectors.push_back(std::make_unique<CollectHandler>(left[i]));
    REQUIRE(r.add(right[i].fd(), *echoes.back(), IoEvent::Read));
    REQUIRE(r.add(left[i].fd(), *collectors.back(), IoEvent::Read));
  }
  for (std::size_t i = 0; i < kPairs; ++i)
    REQUIRE(left[i].write(bytes("m" + std::to_string(i))).ok());
  auto all_done = [&] {
    for (std::size_t i = 0; i < kPairs; ++i) {
      if (collectors[i]->data != "m" + std::to_string(i)) return false;
    }
    return true;
  };
  REQUIRE(run_until(r, all_done, 10000));
  // Remove every other pair, then traffic on the rest still flows.
  for (std::size_t i = 0; i < kPairs; i += 2) {
    REQUIRE(r.remove(left[i].fd()));
    REQUIRE(r.remove(right[i].fd()));
  }
  for (std::size_t i = 1; i < kPairs; i += 2) REQUIRE(left[i].write(bytes("!")).ok());
  REQUIRE(run_until(
      r,
      [&] {
        for (std::size_t i = 1; i < kPairs; i += 2) {
          if (collectors[i]->data.size() != ("m" + std::to_string(i)).size() + 1) return false;
        }
        return true;
      },
      10000));
  for (std::size_t i = 1; i < kPairs; i += 2) {
    r.remove(left[i].fd());
    r.remove(right[i].fd());
  }
}

TEST_CASE("reactor: now_ns is monotonic") {
  const auto a = Reactor::now_ns();
  const auto b = Reactor::now_ns();
  CHECK(b >= a);
  CHECK(a > 0);
}
