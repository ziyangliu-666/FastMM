#include "test_support.hpp"

#include "fastmm/core/engine.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/core/transport.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

using namespace fastmm;

namespace {

struct NoHooks {};

struct NullTransport {
  bool send(const EventHeader&) noexcept { return true; }
  std::size_t send(std::span<const EventHeader* const> batch) noexcept { return batch.size(); }
  bool supports_replace(VenueId) const noexcept { return false; }
};

}  // namespace

TEST_CASE("core.waker: an adaptive engine blocks when idle, wakes for events and for stop()") {
  InstrumentTable table;
  Instrument inst{};
  inst.symbol = "BTCUSDT";
  inst.flags = Instrument::kEnabled;
  inst.tick = 0.01_px;
  inst.lot = 0.001_qty;
  inst.min_qty = 0.001_qty;
  REQUIRE(table.add(inst));
  SimClock clock{Timestamp{seconds(1000).ns}};
  NullTransport transport;
  MsgRing ring(1U << 16);
  RingFeed feed;
  REQUIRE(feed.add_ring(&ring));
  NoHooks strategy;
  EngineConfig cfg;
  cfg.spin_mode = SpinMode::Adaptive;
  cfg.latency_publish_interval = seconds(3600);
  using E = Engine<NoHooks, SimClock, NullTransport, RingFeed>;
  auto engine = std::make_unique<E>(cfg, table, clock, transport, feed, strategy);
  std::thread th([&] { engine->run(); });
  const auto wait_blocked = [&] {
    for (int i = 0; i < 5000 && !feed.waker().waiting(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return feed.waker().waiting();
  };
  CHECK(wait_blocked());
  constexpr int kEvents = 100;
  for (int i = 0; i < kEvents; ++i) {
    ControlMsg m{};
    init_header(m, EventType::Control);
    m.command = ControlCommand::FlushStats;
    REQUIRE(ring.try_push(&m, m.hdr.len));
    feed.notify();
    if (i % 10 == 0) CHECK(wait_blocked());
  }
  CHECK(wait_blocked());
  const auto t0 = std::chrono::steady_clock::now();
  engine->stop();
  th.join();
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
  CHECK(engine->stats().events == kEvents);
}

TEST_CASE("core.waker: notify without a waiting consumer does not block the next wait") {
  Waker w;
  w.notify();  // nobody waiting: no effect
  CHECK_FALSE(w.waiting());
  w.prepare_wait();
  CHECK(w.waiting());
  w.notify();  // takes the flag: the wait returns at once
  CHECK_FALSE(w.waiting());
  const auto t0 = std::chrono::steady_clock::now();
  w.wait(seconds(5));
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
  CHECK_FALSE(w.waiting());
}

TEST_CASE("core.waker: wait times out, cancel_wait clears the flag") {
  Waker w;
  w.prepare_wait();
  w.cancel_wait();
  CHECK_FALSE(w.waiting());
  w.prepare_wait();
  const auto t0 = std::chrono::steady_clock::now();
  w.wait(milliseconds(20));
  const auto waited = std::chrono::steady_clock::now() - t0;
  CHECK(waited >= std::chrono::milliseconds(19));
  CHECK_FALSE(w.waiting());
}

// A consumer that blocks whenever its rings look empty and a producer that publishes then notifies:
// every message is consumed without the consumer's wait timing out (a lost wake-up would stall it
// for the whole timeout).
TEST_CASE("core.waker: RingFeed producer and blocking consumer lose no wake-up") {
  constexpr std::uint64_t kMessages = 20'000;
  MsgRing ring(1U << 16);
  RingFeed feed;
  REQUIRE(feed.add_ring(&ring));
  std::atomic<std::uint64_t> timeouts{0};
  std::thread consumer([&] {
    std::uint64_t seen = 0;
    while (seen < kMessages) {
      if (const EventHeader* h = feed.next()) {
        CHECK(h->seq == seen);
        ++seen;
        feed.release();
        continue;
      }
      feed.waker().prepare_wait();
      if (feed.pending()) {
        feed.waker().cancel_wait();
        continue;
      }
      const auto t0 = std::chrono::steady_clock::now();
      feed.waker().wait(seconds(2));
      if (std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(2)) ++timeouts;
    }
  });
  for (std::uint64_t i = 0; i < kMessages; ++i) {
    ControlMsg m{};
    init_header(m, EventType::Control);
    m.hdr.seq = i;
    while (!ring.try_push(&m, m.hdr.len)) std::this_thread::yield();
    feed.notify();
    if (i % 64 == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  consumer.join();
  CHECK(timeouts.load() == 0);
}
