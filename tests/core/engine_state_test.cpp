// [strategy] state_file: the engine hands EngineConfig::initial_state to the strategy's restore()
// once after on_start, and takes its state() every state_interval and at finish() for whoever runs
// it to write (take_strategy_state). A strategy without state() costs nothing and yields nothing.
#include "test_support.hpp"

#include "fastmm/core/engine.hpp"

#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace fastmm;

namespace {

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

struct Counting {
  static std::string_view name() noexcept { return "counting"; }
  int books = 0;
  int started = 0;
  int books_at_restore = -1;
  std::vector<std::string> restored;
  std::string buf;
  bool accept = true;

  void on_start(auto&) noexcept { ++started; }
  void on_book(auto&, InstrumentId, const auto&) noexcept { ++books; }
  std::string_view state() {
    buf = "books=" + std::to_string(books);
    return buf;
  }
  bool restore(std::string_view bytes) {
    books_at_restore = books;
    restored.emplace_back(bytes);
    if (!accept || !bytes.starts_with("books=")) return false;
    books = std::stoi(std::string(bytes.substr(6)));
    return true;
  }
};
static_assert(KeepsState<Counting>);
static_assert(verify_strategy<Counting>());

struct Stateless {
  static std::string_view name() noexcept { return "stateless"; }
};
static_assert(!KeepsState<Stateless>);

struct Outbox {
  bool send(const EventHeader&) noexcept { return true; }
  std::size_t send(std::span<const EventHeader* const> batch) noexcept { return batch.size(); }
  bool supports_replace(VenueId) const noexcept { return false; }
};

template <class S>
struct Fixture {
  using EngineType = Engine<S, SimClock, Outbox, InlineFeed>;
  InstrumentTable table;
  SimClock clock{Timestamp{seconds(1000).ns}};
  Outbox transport;
  InlineFeed feed{1 << 20};
  S strategy;
  std::unique_ptr<EngineType> engine;

  explicit Fixture(std::string initial = {},
                   const char* state_file = "state.bin",
                   MsgRing* journal = nullptr,
                   Duration snapshot = {}) {
    Instrument i{};
    i.symbol = "BTCUSDT";
    i.venue = VenueId{0};
    i.flags = Instrument::kEnabled;
    i.tick = px("0.01");
    i.lot = qt("0.001");
    REQUIRE(table.add(i));
    EngineConfig cfg;
    cfg.state_file = state_file;
    cfg.state_interval = seconds(10);
    cfg.initial_state = std::move(initial);
    cfg.state_snapshot_interval = snapshot;
    engine = std::make_unique<EngineType>(cfg, table, clock, transport, feed, strategy, journal);
    engine->warm_up();
  }
  void book() {
    std::byte* p = feed.reserve(BookDeltaMsg::size_for(1, 1));
    REQUIRE(p != nullptr);
    auto* d = reinterpret_cast<BookDeltaMsg*>(p);
    init_header(
        *d, EventType::BookSnapshot, InstrumentId{0}, VenueId{0}, BookDeltaMsg::size_for(1, 1));
    d->hdr.flags |= EventHeader::kSnapshot;
    d->hdr.recv_ts = clock.now();
    d->bid_count = d->ask_count = 1;
    d->levels()[0] = Level{px("100.00"), qt("5")};
    d->levels()[1] = Level{px("100.02"), qt("5")};
    feed.commit();
    while (engine->step() != 0) {
    }
  }
  // A position as a venue (or the store's restore) reports it.
  void position(const char* qty, const char* avg_px) {
    ReconcileMsg m{};
    init_header(m, EventType::Reconcile, InstrumentId{0}, VenueId{0});
    m.kind = ReconcileMsg::Kind::Position;
    m.position_qty = qt(qty);
    m.avg_px = px(avg_px);
    m.hdr.recv_ts = clock.now();
    REQUIRE(feed.push(m.hdr));
    while (engine->step() != 0) {
    }
  }
  // Time passes, polled as a live engine polls: the engine's timers fire from step() with nothing
  // in the feed.
  void advance(Duration d) {
    for (Duration left = d; left.ns > 0; left.ns -= milliseconds(100).ns) {
      clock.advance(left.ns < milliseconds(100).ns ? left : milliseconds(100));
      while (engine->step() != 0) {
      }
    }
  }
};

}  // namespace

TEST_CASE("core.engine: the state is restored once after on_start and before the first event") {
  Fixture<Counting> f("books=7");
  f.engine->start();
  CHECK(f.strategy.started == 1);
  CHECK(f.strategy.restored == std::vector<std::string>{"books=7"});
  CHECK(f.strategy.books_at_restore == 0);  // after on_start, before any book
  CHECK(f.strategy.books == 7);
  CHECK(f.engine->state_restored());
  f.book();
  CHECK(f.strategy.books == 8);
  std::string out;
  CHECK_FALSE(f.engine->take_strategy_state(out));  // nothing captured yet
}

TEST_CASE("core.engine: the state is taken every state_interval and at finish") {
  Fixture<Counting> f;
  f.engine->start();
  CHECK(f.strategy.restored.empty());  // no initial state: restore is not called
  CHECK_FALSE(f.engine->state_restored());
  f.book();
  f.book();
  std::string out;
  f.advance(seconds(9));
  CHECK_FALSE(f.engine->take_strategy_state(out));
  f.advance(seconds(1));
  REQUIRE(f.engine->take_strategy_state(out));
  CHECK(out == "books=2");
  CHECK(f.engine->state_captures() == 1);
  CHECK_FALSE(f.engine->take_strategy_state(out));  // taken once
  f.book();
  f.advance(seconds(10));
  REQUIRE(f.engine->take_strategy_state(out));
  CHECK(out == "books=3");
  // The last capture, after on_stop, supersedes one not taken yet.
  f.book();
  f.advance(seconds(10));
  f.book();
  f.engine->finish();
  REQUIRE(f.engine->take_strategy_state(out));
  CHECK(out == "books=5");
  CHECK(f.engine->state_captures() == 4);
}

TEST_CASE("core.engine: state the strategy does not take leaves it as it started") {
  Fixture<Counting> f("garbage");
  f.engine->start();
  CHECK(f.strategy.restored == std::vector<std::string>{"garbage"});
  CHECK(f.strategy.books == 0);
  CHECK_FALSE(f.engine->state_restored());
  f.book();
  CHECK(f.strategy.books == 1);  // trading goes on
}

TEST_CASE("core.engine: without a state_file nothing is captured") {
  Fixture<Counting> f("books=3", "");
  f.engine->start();
  CHECK(f.strategy.books == 3);  // initial_state is still restored
  f.advance(seconds(30));
  f.engine->finish();
  std::string out;
  CHECK_FALSE(f.engine->take_strategy_state(out));
  CHECK(f.engine->state_captures() == 0);
}

TEST_CASE("core.engine: a strategy without state() yields none") {
  Fixture<Stateless> f("anything");
  f.engine->start();
  f.advance(seconds(30));
  f.engine->finish();
  std::string out;
  CHECK_FALSE(f.engine->take_strategy_state(out));
}

TEST_CASE("core.engine: TakeOver restores the staged state after the warm-up and sets the carry") {
  Fixture<Counting> f;  // a warm standby starts without the state file
  f.engine->start();
  CHECK(f.strategy.restored.empty());
  f.book();
  f.book();
  CHECK(f.strategy.books == 2);
  f.engine->stage_strategy_state("books=40");
  ControlMsg m{};
  init_header(m, EventType::Control, InstrumentId{}, VenueId::invalid());
  m.command = ControlCommand::TakeOver;
  m.arg = static_cast<std::uint64_t>(Notional::from_int(-3).raw);
  REQUIRE(f.feed.push(m.hdr));
  while (f.engine->step() != 0) {
  }
  CHECK(f.strategy.restored == std::vector<std::string>{"books=40"});
  CHECK(f.strategy.books_at_restore == 2);  // after the market data it warmed up on
  CHECK(f.strategy.books == 40);
  CHECK(f.engine->net_pnl() == Notional::from_int(-3));
  // The state to write is the restored one, not what the warm-up captured.
  std::string taken;
  REQUIRE(f.engine->take_strategy_state(taken));
  CHECK(taken == "books=40");
  // A TakeOver with nothing staged (a replay) restores nothing.
  REQUIRE(f.feed.push(m.hdr));
  while (f.engine->step() != 0) {
  }
  CHECK(f.strategy.restored.size() == 1);
}

namespace {

// The StrategyState records in a journal ring, in order.
std::vector<std::vector<std::byte>> state_records(MsgRing& ring) {
  std::vector<std::vector<std::byte>> out;
  while (const std::byte* p = ring.try_peek()) {
    const auto* h = reinterpret_cast<const EventHeader*>(p);
    if (h->type == EventType::StrategyState) out.emplace_back(p, p + h->len);
    ring.release();
  }
  return out;
}
const StrategyStateMsg& as_state(const std::vector<std::byte>& r) {
  return *reinterpret_cast<const StrategyStateMsg*>(r.data());
}
std::string bytes_of(const StrategyStateMsg& m) {
  return {reinterpret_cast<const char*>(m.data()), m.bytes};
}

}  // namespace

TEST_CASE("core.engine: the journal gets the restored state and the state snapshots") {
  MsgRing ring(1 << 20);
  Fixture<Counting> f("books=7", "", &ring, seconds(5));
  f.engine->start();
  std::vector<std::vector<std::byte>> recs = state_records(ring);
  REQUIRE(recs.size() == 1);
  const StrategyStateMsg& restored = as_state(recs[0]);
  CHECK(restored.kind == StrategyStateMsg::Kind::Restored);
  CHECK(restored.part == StrategyStateMsg::kStatePart);
  CHECK(restored.flags == StrategyStateMsg::kAccepted);
  CHECK(restored.total == 7);
  CHECK(bytes_of(restored) == "books=7");
  CHECK(restored.hdr.len == 192);

  f.position("2", "100.00");
  f.book();
  f.advance(seconds(5));
  recs = state_records(ring);
  REQUIRE(recs.size() == 2);
  const StrategyStateMsg& state = as_state(recs[0]);
  CHECK(state.kind == StrategyStateMsg::Kind::Snapshot);
  CHECK(state.id == 2);
  CHECK(bytes_of(state) == "books=8");
  const StrategyStateMsg& pos = as_state(recs[1]);
  CHECK(pos.id == 2);
  CHECK(pos.part == StrategyStateMsg::kPositionsPart);
  REQUIRE(pos.bytes == sizeof(StatePosition));
  StatePosition p{};
  std::memcpy(&p, pos.data(), sizeof p);
  CHECK(p.instrument == 0);
  CHECK(p.qty_raw == qt("2").raw);
  CHECK(p.avg_px_raw == px("100.00").raw);
  // Without a state_file nothing is captured for a file; the journal's snapshots go on.
  std::string out;
  CHECK_FALSE(f.engine->take_strategy_state(out));
}

TEST_CASE("core.engine: no snapshots without an interval or a journal, a refused state flagged") {
  MsgRing ring(1 << 20);
  {
    Fixture<Counting> f({}, "", &ring);
    f.engine->start();
    f.advance(seconds(20));
    CHECK(state_records(ring).empty());  // nothing restored, no interval
  }
  {
    Fixture<Counting> f("bogus", "", &ring);
    f.strategy.accept = false;
    f.engine->start();
    const auto recs = state_records(ring);
    REQUIRE(recs.size() == 1);
    CHECK(as_state(recs[0]).flags == 0);  // written, flagged refused
  }
  Fixture<Counting> f("books=1", "", nullptr, seconds(5));
  f.engine->start();
  f.advance(seconds(20));  // no journal: nothing to write to
  CHECK(f.strategy.books == 1);
}
