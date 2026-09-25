// The wake-ups between a gateway and the strategy attached to it, each side in its own process:
// the protocol of tests/core/waker_test.cpp ("RingFeed producer and blocking consumer lose no
// wake-up") across a fork, in both directions. A lost wake-up stalls the consumer for its whole
// timeout (2 s), which the consumer counts.
#include "test_support.hpp"

#include "fastmm/core/messages.hpp"
#include "fastmm/core/shm_ring.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/core/transport.hpp"
#include "fastmm/live/gateway.hpp"
#include "fastmm/net/reactor.hpp"

#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

using namespace fastmm;
namespace gw = fastmm::live::gw;

namespace {

constexpr std::uint64_t kMessages = 20'000;

std::string ring_path(const char* name) {
  return (fastmm::test::tmp_dir() / (std::string(name) + "-" + std::to_string(::getpid())))
      .string();
}

ControlMsg message(std::uint64_t seq) {
  ControlMsg m{};
  init_header(m, EventType::Control);
  m.hdr.seq = seq;
  return m;
}

// The consumer gave up (a stalled producer would spin on a full ring forever): kill it first.
int reap_child(pid_t pid, bool kill) {
  if (kill) ::kill(pid, SIGKILL);
  int status = 0;
  if (::waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

}  // namespace

// Gateway -> engine: the gateway's network thread pushes into a shared ring and notifies its own
// Waker over the engine's flag in the wake page; the engine blocks on its RingFeed's Waker, shared
// into the same page.
TEST_CASE("gateway.wake: the engine blocked on the wake page's futex loses no wake-up") {
  const std::string path = ring_path("wake_md.ring");
  auto ring = ShmRing::create(path, 1U << 16);
  REQUIRE(ring.has_value());
  const int page_fd = gw::create_wake_page();
  REQUIRE(page_fd >= 0);
  gw::WakePage* page = gw::map_wake_page(page_fd);
  REQUIRE(page != nullptr);

  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    // The gateway: its own mapping of the page and of the ring.
    gw::WakePage* mine = gw::map_wake_page(page_fd);
    auto out = ShmRing::open(path);
    if (mine == nullptr || !out) ::_exit(10);
    Waker engine;
    engine.share(&mine->engine.flag);
    for (std::uint64_t i = 0; i < kMessages; ++i) {
      const ControlMsg m = message(i);
      while (!out->try_push(&m, m.hdr.len)) std::this_thread::yield();
      engine.notify();
      if (i % 64 == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    ::_exit(0);
  }

  RingFeed feed;
  REQUIRE(feed.add_ring(&*ring));
  feed.waker().share(&page->engine.flag);
  std::uint64_t seen = 0;
  std::uint64_t waits = 0;
  std::uint64_t timeouts = 0;
  bool in_order = true;
  while (seen < kMessages && timeouts < 3) {
    if (const EventHeader* h = feed.next()) {
      in_order = in_order && h->seq == seen;
      ++seen;
      feed.release();
      continue;
    }
    feed.waker().prepare_wait();
    if (feed.pending()) {
      feed.waker().cancel_wait();
      continue;
    }
    ++waits;
    const auto t0 = std::chrono::steady_clock::now();
    feed.waker().wait(seconds(2));
    if (std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(2)) ++timeouts;
  }
  CHECK(reap_child(child, seen < kMessages) == 0);
  CHECK(seen == kMessages);
  CHECK(in_order);
  CHECK(timeouts == 0);
  CHECK(waits > 0);  // the engine did block, so the futex was exercised
  gw::unmap_wake_page(page);
  ::close(page_fd);
}

// Engine -> gateway: the gateway's network thread sets its flag in the wake page, rechecks the
// outbound ring and blocks in its reactor; the engine, holding the page and the reactor's eventfd
// as the attach reply passes them (SCM_RIGHTS), pushes and calls wake_if_blocked.
TEST_CASE("gateway.wake: a network thread blocked in its reactor loses no wake-up") {
  const std::string path = ring_path("wake_out.ring");
  auto ring = ShmRing::create(path, 1U << 16);
  REQUIRE(ring.has_value());
  int sp[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sp) == 0);

  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    // The engine: receives the descriptors, then produces.
    ::close(sp[0]);
    char b = 0;
    int fds[2];
    std::size_t nfds = 0;
    if (gw::recv_with_fds(sp[1], &b, 1, fds, 2, &nfds) != 1 || nfds != 2) ::_exit(10);
    gw::WakePage* page = gw::map_wake_page(fds[0]);
    auto out = ShmRing::open(path);
    if (page == nullptr || !out) ::_exit(11);
    for (std::uint64_t i = 0; i < kMessages; ++i) {
      const ControlMsg m = message(i);
      while (!out->try_push(&m, m.hdr.len)) std::this_thread::yield();
      gw::wake_if_blocked(page->net[0].flag, fds[1]);
      if (i % 64 == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    ::_exit(0);
  }
  ::close(sp[1]);

  net::Reactor reactor(net::ReactorBackend::Epoll);
  const int page_fd = gw::create_wake_page();
  REQUIRE(page_fd >= 0);
  gw::WakePage* page = gw::map_wake_page(page_fd);
  REQUIRE(page != nullptr);
  const int fds[2] = {page_fd, reactor.wake_fd()};
  const char b = 'w';
  REQUIRE(gw::send_with_fds(sp[0], &b, 1, fds) == 1);
  ::close(sp[0]);

  // net_loop's block (src/live/venue_slot.cpp) with gateway_pending's recheck.
  SleepFlag& blocked = page->net[0].flag;
  std::uint64_t seen = 0;
  std::uint64_t waits = 0;
  std::uint64_t timeouts = 0;
  bool in_order = true;
  while (seen < kMessages && timeouts < 3) {
    if (const std::byte* p = ring->try_peek()) {
      in_order = in_order && reinterpret_cast<const EventHeader*>(p)->seq == seen;
      ++seen;
      ring->release();
      continue;
    }
    blocked.set();
    if (!ring->empty_approx()) {
      blocked.clear();
      continue;
    }
    ++waits;
    const auto t0 = std::chrono::steady_clock::now();
    static_cast<void>(reactor.run_once(2000));
    blocked.clear();
    if (std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(2)) ++timeouts;
  }
  CHECK(reap_child(child, seen < kMessages) == 0);
  CHECK(seen == kMessages);
  CHECK(in_order);
  CHECK(timeouts == 0);
  CHECK(waits > 0);
  gw::unmap_wake_page(page);
  ::close(page_fd);
}
