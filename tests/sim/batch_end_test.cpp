// on_batch_end (ctx.request_batch_end): one call after the events waiting in the feed, not one per
// event; journaled, so a replay calls it after the same event and sends the same orders.
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

// Quotes the latest book once per batch: on_book only asks for the batch end.
struct Batcher {
  int books = 0;
  std::vector<int> books_at_end;  // on_book calls before each on_batch_end
  bool again = false;             // ask once more from inside on_batch_end

  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId, const Book&) noexcept {
    ++books;
    ctx.request_batch_end();
  }
  template <class Ctx>
  void on_batch_end(Ctx& ctx) noexcept {
    books_at_end.push_back(books);
    if (again) {
      again = false;
      ctx.request_batch_end();
    }
    const auto& b = ctx.book(kId);
    if (!b.is_valid()) return;
    DesiredQuotes q;
    q.bid(b.best_bid().price, qt("0.01"));
    q.ask(b.best_ask().price, qt("0.01"));
    static_cast<void>(ctx.set_quotes(kId, q));
  }
};
static_assert(verify_strategy<Batcher>());

struct Quiet {
  int ends = 0;
  template <class Ctx>
  void on_batch_end(Ctx&) noexcept {
    ++ends;
  }
};

void push_book(InlineFeed& feed, SimClock& clock, const char* bid, const char* ask) {
  alignas(64) std::byte buf[BookDeltaMsg::size_for(1, 1)];
  auto* d = reinterpret_cast<BookDeltaMsg*>(buf);
  init_header(*d, EventType::BookSnapshot, kId, VenueId{0}, sizeof buf);
  d->hdr.flags |= EventHeader::kSnapshot;
  d->hdr.recv_ts = clock.now();
  d->bid_count = d->ask_count = 1;
  d->levels()[0] = Level{px(bid), qt("1")};
  d->levels()[1] = Level{px(ask), qt("1")};
  REQUIRE(feed.push(d->hdr));
}

}  // namespace

TEST_CASE("sim.batch_end: one call after the waiting events, journaled and replayed alike") {
  const auto path = (tmp_dir() / "batch_end.fmj").string();
  const InstrumentTable instruments = table();
  std::string recorded_hash;
  std::uint64_t recorded_count = 0;
  std::vector<int> recorded_ends;
  {
    MsgRing ring(1 << 20);
    JournalSessionInfo info;
    info.strategy = "batcher";
    info.instruments = &instruments;
    info.has_session = true;
    info.session_epoch = 1;
    JournalFileWriter fw(ring, path, info);
    REQUIRE(fw.ok());
    SimClock clock(Timestamp{1'700'000'000'000'000'000LL});
    HashTransport transport;
    InlineFeed feed(1 << 16);
    Batcher strategy;
    Engine<Batcher, SimClock, HashTransport, InlineFeed> engine(
        engine_config(), instruments, clock, transport, feed, strategy, &ring);
    engine.warm_up();
    engine.start();

    // Three books waiting: three on_book calls, then one on_batch_end that quotes the last one.
    push_book(feed, clock, "100.00", "100.10");
    push_book(feed, clock, "100.01", "100.11");
    push_book(feed, clock, "100.02", "100.12");
    clock.advance(milliseconds(1));
    CHECK(engine.step() == 3);
    CHECK(strategy.books == 3);
    REQUIRE(strategy.books_at_end.size() == 1);
    CHECK(strategy.books_at_end[0] == 3);
    CHECK(engine.stats().orders_sent == 2);

    // Nothing asked: no call.
    clock.advance(milliseconds(1));
    CHECK(engine.step() == 0);
    CHECK(strategy.books_at_end.size() == 1);

    // A request from inside on_batch_end makes one more call, after the step's timers, not a loop.
    strategy.again = true;
    push_book(feed, clock, "100.03", "100.13");
    clock.advance(milliseconds(1));
    CHECK(engine.step() == 1);
    CHECK(strategy.books_at_end.size() == 3);
    clock.advance(milliseconds(1));
    CHECK(engine.step() == 0);
    CHECK(strategy.books_at_end.size() == 3);
    CHECK(engine.stats().batch_ends == 3);

    engine.finish();
    fw.drain_once();
    fw.stop();
    recorded_hash = transport.hash();
    recorded_count = transport.count();
    recorded_ends = strategy.books_at_end;
  }
  CHECK(recorded_count >= 2);

  // The replay steps one journal event at a time: its batch ends are the journal's markers.
  JournalReader reader;
  REQUIRE(reader.open(path));
  std::size_t markers = 0;
  reader.for_each([&](const EventHeader* h) {
    if (h->type == EventType::Timer && (h->flags & EventHeader::kOutbound) == 0 &&
        msg_cast<TimerMsg>(h).engine ==
            Engine<Batcher, SimClock, HashTransport, InlineFeed>::kBatchEndTimer)
      ++markers;
  });
  CHECK(markers == 3);
  sim::ReplayTransport rt;
  rt.set_expected(sim::ReplayTransport::load_outbound(reader));
  sim::JournalFeed jf(reader);
  SimClock rclock(Timestamp{reader.header().start_ts_ns});
  Batcher rstrategy;
  Engine<Batcher, SimClock, sim::ReplayTransport, sim::JournalFeed> replay(
      engine_config(), instruments, rclock, rt, jf, rstrategy);
  sim::ReplayDriver driver(rclock, jf, sim::EngineHooks::for_engine(replay));
  driver.run_all();
  driver.finish();
  CHECK(rt.first_mismatch() == -1);
  CHECK(rt.count() == recorded_count);
  CHECK(rt.hash_hex() == recorded_hash);
  CHECK(rstrategy.books_at_end == recorded_ends);
}

// A strategy that never asks pays nothing and journals no marker.
TEST_CASE("sim.batch_end: no request, no call") {
  const InstrumentTable instruments = table();
  SimClock clock(Timestamp{1'700'000'000'000'000'000LL});
  HashTransport transport;
  InlineFeed feed(1 << 16);
  Quiet strategy;
  Engine<Quiet, SimClock, HashTransport, InlineFeed> engine(
      engine_config(), instruments, clock, transport, feed, strategy);
  engine.warm_up();
  engine.start();
  push_book(feed, clock, "100.00", "100.10");
  CHECK(engine.step() == 1);
  CHECK(strategy.ends == 0);
  CHECK(engine.stats().batch_ends == 0);
}
