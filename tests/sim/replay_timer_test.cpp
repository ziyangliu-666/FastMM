// A timer a strategy adds during the session has the same id in the replay: the replay fires the
// journal's timers in the engine's wheel instead of polling it, so the ids the wheel hands out
// follow the session's, and a strategy that tells its timers apart by id sends the same orders.
#include "test_support.hpp"

#include "fastmm/core/engine.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/sim/journal_feed.hpp"
#include "fastmm/sim/outbound_hash.hpp"
#include "fastmm/sim/replay_transport.hpp"
#include "fastmm/sim/sim_driver.hpp"

#include <span>
#include <string>
#include <vector>

using namespace fastmm;
using fastmm::test::tmp_dir;

namespace {

constexpr InstrumentId kId{0};

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

InstrumentTable table() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.venue = VenueId{0};
  i.flags = Instrument::kEnabled;
  i.tick = px("0.01");
  i.lot = qt("0.001");
  i.min_qty = i.lot;
  REQUIRE(t.add(i));
  return t;
}

EngineConfig engine_config() {
  EngineConfig ec;
  ec.quotes.min_requote_interval = Duration{};
  ec.risk.max_order_qty = qt("1");
  ec.risk.max_position = qt("10");
  ec.risk.max_open_orders = 8;
  return ec;
}

class HashTransport {
 public:
  [[nodiscard]] bool send(const EventHeader& m) noexcept {
    hasher_.add(m);
    return true;
  }
  [[nodiscard]] std::size_t send(std::span<const EventHeader* const> batch) noexcept {
    for (const EventHeader* m : batch) hasher_.add(*m);
    return batch.size();
  }
  [[nodiscard]] bool supports_replace(VenueId) const noexcept { return false; }
  [[nodiscard]] std::string hash() const { return hasher_.hex(); }
  [[nodiscard]] std::uint64_t count() const noexcept { return hasher_.count(); }

 private:
  sim::OutboundHasher hasher_;
};

// A book starts a one-shot delay; when that timer (and no other) fires, it quotes the book. A
// second, repeating timer started in on_start shares the wheel.
struct Delayed {
  TimerId heartbeat;
  TimerId pending;
  int quotes = 0;
  int heartbeats = 0;
  std::vector<std::uint32_t> ids;  // of every timer that fired

  template <class Ctx>
  void on_start(Ctx& ctx) noexcept {
    heartbeat = ctx.every(milliseconds(5), 1);
  }
  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId, const Book&) noexcept {
    if (!pending.valid()) pending = ctx.once(milliseconds(1), 2);
  }
  template <class Ctx>
  void on_timer(Ctx& ctx, TimerId id, std::uint64_t) noexcept {
    ids.push_back(id.value);
    if (id == heartbeat) {
      ++heartbeats;
      return;
    }
    if (id != pending) return;
    pending = TimerId{};
    const auto& b = ctx.book(kId);
    if (!b.is_valid()) return;
    ++quotes;
    DesiredQuotes q;
    q.bid(b.best_bid().price, qt("0.01"));
    q.ask(b.best_ask().price, qt("0.01"));
    static_cast<void>(ctx.set_quotes(kId, q));
  }
};
static_assert(verify_strategy<Delayed>());

void push_book(InlineFeed& feed, SimClock& clock, std::int64_t bid_cents) {
  alignas(64) std::byte buf[BookDeltaMsg::size_for(1, 1)];
  auto* d = reinterpret_cast<BookDeltaMsg*>(buf);
  init_header(*d, EventType::BookSnapshot, kId, VenueId{0}, sizeof buf);
  d->hdr.flags |= EventHeader::kSnapshot;
  d->hdr.recv_ts = clock.now();
  d->bid_count = d->ask_count = 1;
  d->levels()[0] = Level{Price::from_raw(bid_cents * (kFixedScale / 100)), qt("1")};
  d->levels()[1] = Level{Price::from_raw((bid_cents + 10) * (kFixedScale / 100)), qt("1")};
  REQUIRE(feed.push(d->hdr));
}

}  // namespace

TEST_CASE("sim.replay_timer: timers added during the session have the session's ids") {
  const auto path = (tmp_dir() / "replay_timer.fmj").string();
  const InstrumentTable instruments = table();
  std::string recorded_hash;
  std::uint64_t recorded_count = 0;
  std::vector<std::uint32_t> recorded_ids;
  int recorded_quotes = 0;
  {
    MsgRing ring(1 << 20);
    JournalSessionInfo info;
    info.strategy = "delayed";
    info.instruments = &instruments;
    info.has_session = true;
    info.session_epoch = 1;
    JournalFileWriter fw(ring, path, info);
    REQUIRE(fw.ok());
    SimClock clock(Timestamp{1'700'000'000'000'000'000LL});
    HashTransport transport;
    InlineFeed feed(1 << 16);
    Delayed strategy;
    Engine<Delayed, SimClock, HashTransport, InlineFeed> engine(
        engine_config(), instruments, clock, transport, feed, strategy, &ring);
    engine.warm_up();
    engine.start();
    for (int i = 0; i < 20; ++i) {
      push_book(feed, clock, 10'000 + i);
      clock.advance(microseconds(700));
      static_cast<void>(engine.step());
      clock.advance(microseconds(700));
      static_cast<void>(engine.step());
    }
    engine.finish();
    fw.drain_once();
    fw.stop();
    recorded_hash = transport.hash();
    recorded_count = transport.count();
    recorded_ids = strategy.ids;
    recorded_quotes = strategy.quotes;
    CHECK(strategy.heartbeats > 0);
  }
  CHECK(recorded_quotes >= 10);

  JournalReader reader;
  REQUIRE(reader.open(path));
  sim::ReplayTransport rt;
  rt.set_expected(sim::ReplayTransport::load_outbound(reader));
  sim::JournalFeed jf(reader);
  SimClock rclock(Timestamp{reader.header().start_ts_ns});
  Delayed rstrategy;
  Engine<Delayed, SimClock, sim::ReplayTransport, sim::JournalFeed> replay(
      engine_config(), instruments, rclock, rt, jf, rstrategy);
  sim::ReplayDriver driver(rclock, jf, sim::EngineHooks::for_engine(replay));
  driver.run_all();
  driver.finish();
  CHECK(rstrategy.ids == recorded_ids);
  CHECK(rstrategy.quotes == recorded_quotes);
  CHECK(rt.first_mismatch() == -1);
  CHECK(rt.count() == recorded_count);
  CHECK(rt.hash_hex() == recorded_hash);
}
