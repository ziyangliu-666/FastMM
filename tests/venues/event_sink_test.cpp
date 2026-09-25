// EventSink drain hook (run-to-completion, [engine] threading = "single"): with on_commit the hook
// runs after every commit; on a full ring it runs before giving up, instead of the spin.
#include "fastmm/venues/event_sink.hpp"

#include "test_support.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

using namespace fastmm;
using fastmm::venues::EventSink;
using fastmm::venues::SinkPolicy;

namespace {

struct Consumer {
  MsgRing* ring = nullptr;
  std::size_t calls = 0;
  std::size_t taken = 0;
  bool consume = true;
};

void drain(void* ctx) noexcept {
  auto* c = static_cast<Consumer*>(ctx);
  ++c->calls;
  if (!c->consume) return;
  while (c->ring->try_peek() != nullptr) {
    c->ring->release();
    ++c->taken;
  }
}

TradeMsg trade() {
  TradeMsg t{};
  init_header(t, EventType::Trade, InstrumentId{0}, VenueId{0});
  return t;
}

}  // namespace

TEST_CASE("venues.event_sink: a drain hook on commit consumes each event as it is committed") {
  MsgRing ring(1U << 12);
  Consumer c{&ring};
  EventSink sink(&ring, SinkPolicy::Drop);
  sink.set_drain_hook(&drain, &c, /*on_commit=*/true);
  const TradeMsg t = trade();
  for (int i = 0; i < 100; ++i) REQUIRE(sink.push(t.hdr));  // far more than the ring holds
  CHECK(c.calls == 100);
  CHECK(c.taken == 100);
  CHECK(sink.overflows() == 0);
  CHECK(ring.empty_approx());
}

TEST_CASE("venues.event_sink: without on_commit the hook runs only when the ring is full") {
  MsgRing ring(1U << 12);
  Consumer c{&ring};
  EventSink sink(&ring, SinkPolicy::Spin);
  sink.set_drain_hook(&drain, &c, /*on_commit=*/false);
  const TradeMsg t = trade();
  const std::size_t fits = ring.capacity() / ((sizeof(TradeMsg) + 63) / 64 * 64);
  for (std::size_t i = 0; i < fits; ++i) REQUIRE(sink.push(t.hdr));
  CHECK(c.calls == 0);
  REQUIRE(sink.push(t.hdr));  // full: drained, then taken
  CHECK(c.calls == 1);
  CHECK(c.taken == fits);
  // A hook that frees nothing: the push fails at once (no spin: the consumer is this thread).
  c.consume = false;
  for (std::size_t i = 1; i < fits; ++i) REQUIRE(sink.push(t.hdr));
  CHECK_FALSE(sink.push(t.hdr));
  CHECK(sink.overflows() == 1);
  sink.set_drain_hook(nullptr, nullptr, true);
  CHECK(c.calls == 2);
}

// fastmm-gateway moves a venue's sinks between a strategy's shared ring and a local ring it
// discards: each attach takes effect at the next push, and the shared ring reads what was pushed.
TEST_CASE("venues.event_sink: a sink switches between a local ring and a shared ring") {
  const std::string path = (fastmm::test::tmp_dir() / "event_sink_switch.ring").string();
  auto shm = ShmRing::create(path, 1U << 12);
  REQUIRE(shm.has_value());
  MsgRing local(1U << 12);
  EventSink sink(&local, SinkPolicy::Spin);
  const TradeMsg t = trade();
  REQUIRE(sink.push(t.hdr));
  sink.attach(&*shm, SinkPolicy::Spin);
  CHECK(sink.attached());
  CHECK(sink.ring() == nullptr);
  CHECK(sink.shm_ring() == &*shm);
  REQUIRE(sink.push(t.hdr));
  REQUIRE(sink.push(t.hdr));
  sink.attach(&local, SinkPolicy::Spin);
  REQUIRE(sink.push(t.hdr));
  CHECK(sink.pushed() == 4);
  std::size_t in_local = 0;
  while (local.try_peek() != nullptr) {
    local.release();
    ++in_local;
  }
  std::size_t in_shm = 0;
  while (const std::byte* p = shm->try_peek()) {
    CHECK(reinterpret_cast<const EventHeader*>(p)->type == EventType::Trade);
    shm->release();
    ++in_shm;
  }
  CHECK(in_local == 2);
  CHECK(in_shm == 2);
  std::filesystem::remove(path);
}
