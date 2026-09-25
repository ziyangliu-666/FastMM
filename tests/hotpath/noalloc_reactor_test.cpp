// Reactor::run_once / dispatch must not allocate once fds are registered, on both backends, and
// neither must arming, firing or cancelling timers once the timer table has reached its size.
// Registration and post() may allocate and stay outside the measured scope.
#if defined(FASTMM_HOTPATH_NET)
#include "alloc_counter.hpp"
#include "test_support.hpp"

#include "fastmm/net/reactor.hpp"
#include "fastmm/net/tcp_socket.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

using namespace fastmm::net;

namespace {

class Echo final : public IoHandler {
 public:
  explicit Echo(TcpSocket& s) : sock_(s) {}
  void on_readable() override {
    std::array<std::byte, 256> buf{};
    for (;;) {
      const IoResult r = sock_.read(buf);
      if (r.bytes > 0) sock_.write(std::span<const std::byte>(buf.data(), r.bytes));
      if (r.bytes == 0) return;
    }
  }
  void on_writable() override { ++writable; }
  void on_error(int) override {}
  int writable = 0;

 private:
  TcpSocket& sock_;
};

class Counter final : public IoHandler {
 public:
  explicit Counter(TcpSocket& s) : sock_(s) {}
  void on_readable() override {
    std::array<std::byte, 256> buf{};
    for (;;) {
      const IoResult r = sock_.read(buf);
      received += r.bytes;
      if (r.bytes == 0) return;
    }
  }
  void on_writable() override {}
  void on_error(int) override {}
  std::size_t received = 0;

 private:
  TcpSocket& sock_;
};

// Echo round trips, write-interest toggles and wake-ups; returns the allocations counted.
std::uint64_t allocations_during_traffic(ReactorBackend backend, bool busy) {
  Reactor r(backend);
  r.set_busy_poll(busy);
  TcpSocket a;
  TcpSocket b;
  REQUIRE(TcpSocket::make_pair(a, b));
  Echo echo(b);
  Counter counter(a);
  REQUIRE(r.add(b.fd(), echo, IoEvent::Read));
  REQUIRE(r.add(a.fd(), counter, IoEvent::Read));
  const std::array<std::byte, 16> msg{};
  std::size_t expected = 0;
  auto round_trip = [&] {
    static_cast<void>(a.write(msg));
    expected += msg.size();
    for (int spins = 0; counter.received < expected && spins < 100'000; ++spins) r.run_once(10);
  };
  for (int i = 0; i < 64; ++i) round_trip();  // warm up

  std::uint64_t allocations = 0;
  {
    fastmm::test::NoAllocScope guard;
    for (int i = 0; i < 2000; ++i) {
      round_trip();
      if (i % 100 == 0) {
        static_cast<void>(r.modify(b.fd(), echo, IoEvent::ReadWrite));
        static_cast<void>(r.modify(b.fd(), echo, IoEvent::Read));
        r.wake();
      }
    }
    allocations = guard.allocations_so_far();
  }
  CHECK(counter.received == expected);
  r.remove(a.fd());
  r.remove(b.fd());
  return allocations;
}

}  // namespace

TEST_CASE("hotpath.noalloc: reactor run_once over epoll") {
  CHECK(allocations_during_traffic(ReactorBackend::Epoll, false) == 0);
  CHECK(allocations_during_traffic(ReactorBackend::Epoll, true) == 0);
}

// The venues' housekeeping timer re-arms itself with [this, weak_ptr] (24 bytes of captures);
// connections cancel and re-arm long timers (heartbeat, health) on every reconnect or pong.
TEST_CASE("hotpath.noalloc: reactor timers re-armed from their callbacks and cancelled") {
  Reactor r;
  const auto alive = std::make_shared<int>(0);
  int fired = 0;
  struct Rearm {
    Reactor* r;
    int* fired;
    std::weak_ptr<int> alive;
    void operator()() const {
      if (alive.expired()) return;
      ++*fired;
      static_cast<void>(r->add_timer_after(0, Rearm{r, fired, alive}));
    }
  };
  static_cast<void>(r.add_timer_after(0, Rearm{&r, &fired, alive}));
  TimerId pending = kInvalidTimer;
  auto iteration = [&] {
    r.run_once(0);
    r.cancel_timer(pending);
    pending = r.add_timer_after(1'000'000'000, [&fired] { ++fired; });
  };
  for (int i = 0; i < 64; ++i) iteration();  // warm up: slots, free list and heap reach their size
  const int before = fired;
  std::uint64_t allocations = 0;
  {
    fastmm::test::NoAllocScope guard;
    for (int i = 0; i < 2000; ++i) iteration();
    allocations = guard.allocations_so_far();
  }
  CHECK(allocations == 0);
  CHECK(fired - before == 2000);
  CHECK(r.active_timers() == 2);
}

TEST_CASE("hotpath.noalloc: reactor run_once over io_uring") {
  if (!Reactor::io_uring_supported()) {
    MESSAGE("io_uring is not available on this kernel, test skipped");
    return;
  }
  CHECK(allocations_during_traffic(ReactorBackend::IoUring, false) == 0);
  CHECK(allocations_during_traffic(ReactorBackend::IoUring, true) == 0);
}
#endif
