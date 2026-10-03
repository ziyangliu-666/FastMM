// [strategy] state_file: the engine hands EngineConfig::initial_state to the strategy's restore()
// once after on_start, and takes its state() every state_interval and at finish() for whoever runs
// it to write (take_strategy_state). A strategy without state() costs nothing and yields nothing.
#include "test_support.hpp"

#include "fastmm/core/engine.hpp"

#include <cstddef>
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

  explicit Fixture(std::string initial = {}, const char* state_file = "state.bin") {
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
    engine = std::make_unique<EngineType>(cfg, table, clock, transport, feed, strategy, nullptr);
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
