// [[windows]] in the engine (core/windows.hpp): a window opens and closes on the engine's clock, by
// its own timer when no event comes, pulls or holds its scope while open, and leaves the
// operator's pulls and holds alone.
#include "test_support.hpp"

#include "fastmm/core/engine.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace fastmm;

namespace {

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

constexpr InstrumentId kA{0};  // venue 0
constexpr InstrumentId kB{1};  // venue 1

// 2026-03-08 20:00 New York is 2026-03-09 00:00Z: daylight saving time began that morning, so an
// EST clock would open the window an hour late.
constexpr std::int64_t kSundayOpenEdt = 1773014400;

InstrumentTable make_table() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.venue = VenueId{0};
  i.flags = Instrument::kEnabled;
  i.tick = px("0.01");
  i.lot = qt("0.001");
  i.min_qty = qt("0.001");
  REQUIRE(t.add(i));
  i.symbol = "ETHUSDT";
  i.venue = VenueId{1};
  REQUIRE(t.add(i));
  return t;
}

struct Transport {
  std::vector<std::vector<std::byte>> out;
  bool send(const EventHeader& m) noexcept {
    const auto* b = reinterpret_cast<const std::byte*>(&m);
    out.emplace_back(b, b + m.len);
    return true;
  }
  std::size_t send(std::span<const EventHeader* const> batch) noexcept {
    for (const EventHeader* m : batch) static_cast<void>(send(*m));
    return batch.size();
  }
  [[nodiscard]] bool supports_replace(VenueId) const noexcept { return false; }
  [[nodiscard]] std::vector<OutNewOrderMsg> news() const {
    std::vector<OutNewOrderMsg> v;
    for (const auto& m : out) {
      const auto* h = reinterpret_cast<const EventHeader*>(m.data());
      if (h->type == EventType::OutNewOrder)
        v.push_back(*reinterpret_cast<const OutNewOrderMsg*>(h));
    }
    return v;
  }
  [[nodiscard]] std::vector<OutCancelMsg> cancels() const {
    std::vector<OutCancelMsg> v;
    for (const auto& m : out) {
      const auto* h = reinterpret_cast<const EventHeader*>(m.data());
      if (h->type == EventType::OutCancel) v.push_back(*reinterpret_cast<const OutCancelMsg*>(h));
    }
    return v;
  }
};

struct Quoter {
  std::vector<bool> accepted;
  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book& b) noexcept {
    if (!b.is_valid()) return;
    DesiredQuotes q;
    static_cast<void>(q.bids.push_back(Level{b.best_bid().price, qt("0.010")}));
    static_cast<void>(q.asks.push_back(Level{b.best_ask().price, qt("0.010")}));
    accepted.push_back(ctx.set_quotes(id, q));
  }
};

TimeZone new_york() {
  std::string why;
  std::optional<TimeZone> z = TimeZone::find("America/New_York", &why);
  REQUIRE(z.has_value());
  return *z;
}

// Sunday 20:00 New York, 30 minutes.
WindowRule sunday_reopen(WindowAction action) {
  WindowRule w;
  w.name = "reopen";
  w.zone = new_york();
  w.weekdays = *parse_weekday("sun");
  w.start_s = 20 * 3600;
  w.duration_s = 30 * 60;
  w.action = action;
  return w;
}

struct Rig {
  using E = Engine<Quoter, SimClock, Transport, InlineFeed>;
  InstrumentTable table = make_table();
  SimClock clock;
  Transport transport;
  InlineFeed feed{1 << 20};
  Quoter strategy;
  std::unique_ptr<E> engine;

  Rig(std::int64_t start_s, std::vector<WindowRule> windows)
      : clock(Timestamp{start_s * 1'000'000'000}) {
    EngineConfig cfg;
    cfg.risk.max_open_orders = 8;
    cfg.quotes.min_requote_interval = Duration{};
    cfg.windows.rules = std::move(windows);
    engine = std::make_unique<E>(cfg, table, clock, transport, feed, strategy);
    engine->warm_up();
    engine->start();
  }
  void drain() {
    while (engine->step() > 0) {
    }
  }
  template <class M>
  void push(M& m) {
    m.hdr.recv_ts = clock.now();
    REQUIRE(feed.push(m.hdr));
    drain();
  }
  void book(InstrumentId id, const char* bid, const char* ask) {
    std::byte* p = feed.reserve(BookDeltaMsg::size_for(1, 1));
    REQUIRE(p != nullptr);
    auto* d = reinterpret_cast<BookDeltaMsg*>(p);
    init_header(*d, EventType::BookSnapshot, id, table.get(id).venue, BookDeltaMsg::size_for(1, 1));
    d->hdr.flags |= EventHeader::kSnapshot;
    d->hdr.recv_ts = clock.now();
    d->bid_count = d->ask_count = 1;
    d->last_update_id = 1;
    d->levels()[0] = Level{px(bid), qt("5")};
    d->levels()[1] = Level{px(ask), qt("5")};
    feed.commit();
    drain();
  }
  void control(ControlCommand c,
               InstrumentId inst = InstrumentId::invalid(),
               VenueId venue = VenueId::invalid(),
               std::uint64_t arg = 0) {
    ControlMsg m{};
    init_header(m, EventType::Control, inst, venue);
    m.command = c;
    m.arg = arg;
    push(m);
  }
  void ack_all() {
    for (const OutNewOrderMsg& n : transport.news()) {
      OrderAckMsg a{};
      init_header(a, EventType::OrderAck, n.hdr.instrument, n.hdr.venue);
      a.cl_ord_id = n.cl_ord_id;
      a.venue_order_id = FixedString<40>(std::to_string(n.cl_ord_id.value).c_str());
      push(a);
    }
  }
  void fill(InstrumentId id, Side side, const char* price, const char* qty) {
    OrderFillMsg m{};
    init_header(m, EventType::OrderFill, id, table.get(id).venue);
    m.cl_ord_id = ClientOrderId{0xF00D};
    m.side = side;
    m.price = px(price);
    m.qty = qt(qty);
    m.exec_id = FixedString<40>("x1");
    push(m);
  }
  void tick(Duration d) {
    clock.advance(d);
    drain();
  }
};

Side side_of(const Rig& r, ClientOrderId id) {
  for (const OutNewOrderMsg& n : r.transport.news())
    if (n.cl_ord_id == id) return n.side;
  FAIL("no new order " << id.value);
  return Side::Buy;
}

}  // namespace

TEST_CASE("core.engine.windows: a pull window opens on its timer at local time and closes again") {
  WindowRule w = sunday_reopen(WindowAction::Pull);
  w.instruments = {kA};
  Rig r(kSundayOpenEdt - 3600, {w});  // 19:00 EDT
  CHECK(r.engine->windows_open() == 0);
  CHECK(r.engine->window_edge(0) == kSundayOpenEdt);
  r.book(kA, "100.00", "100.02");
  r.book(kB, "200.00", "200.02");
  CHECK(r.strategy.accepted == std::vector<bool>{true, true});
  r.ack_all();
  const std::size_t cancels = r.transport.cancels().size();

  r.tick(seconds(3599));  // 19:59:59 EDT: still closed
  CHECK(r.engine->windows_open() == 0);
  CHECK(r.engine->quoting_enabled(kA));
  r.tick(seconds(1));  // 20:00 EDT, no event: the window's timer opens it
  CHECK(r.engine->windows_open() == 1);
  CHECK_FALSE(r.engine->quoting_enabled(kA));
  CHECK(r.engine->quoting_enabled(kB));
  CHECK(r.engine->stats().window_switches == 1);
  CHECK(r.transport.cancels().size() == cancels + 2);  // kA's bid and ask
  CHECK(r.engine->window_edge(0) == kSundayOpenEdt + 1800);

  r.book(kA, "100.00", "100.02");
  CHECK_FALSE(r.strategy.accepted.back());
  // The operator's resume of the whole session leaves the schedule's pull in place.
  r.control(ControlCommand::ResumeQuotes);
  CHECK_FALSE(r.engine->quoting_enabled(kA));
  // An operator pull of kA outlasts the window.
  r.control(ControlCommand::PullQuotes, kA);
  r.tick(seconds(1800));
  CHECK(r.engine->windows_open() == 0);
  CHECK(r.engine->stats().window_switches == 2);
  CHECK_FALSE(r.engine->quoting_enabled(kA));
  r.control(ControlCommand::ResumeQuotes, kA);
  CHECK(r.engine->quoting_enabled(kA));
  r.book(kA, "100.00", "100.02");
  CHECK(r.strategy.accepted.back());
  // The next is a week on, in the same local time.
  CHECK(r.engine->window_edge(0) == kSundayOpenEdt + 7 * 86400);
}

TEST_CASE("core.engine.windows: a window open at the start holds before on_start") {
  WindowRule w = sunday_reopen(WindowAction::Pull);
  w.venues = 1U << 1;  // venue 1: kB
  Rig r(kSundayOpenEdt + 60, {w});
  CHECK(r.engine->windows_open() == 1);
  CHECK(r.engine->quoting_enabled(kA));
  CHECK_FALSE(r.engine->quoting_enabled(kB));
  r.book(kB, "200.00", "200.02");
  CHECK(r.strategy.accepted == std::vector<bool>{false});
}

TEST_CASE(
    "core.engine.windows: a reduce_only window cancels what adds and outlasts reduce-only off") {
  Rig r(kSundayOpenEdt - 60, {sunday_reopen(WindowAction::ReduceOnly)});
  r.book(kA, "100.00", "100.02");
  r.fill(kA, Side::Buy, "100.00", "0.500");  // long: the bid adds
  r.ack_all();
  const std::size_t cancels = r.transport.cancels().size();
  CHECK_FALSE(r.engine->reduce_only(kA));

  r.tick(seconds(60));
  CHECK(r.engine->windows_open() == 1);
  CHECK(r.engine->reduce_only(kA));
  CHECK(r.engine->reduce_only(kB));  // the whole session
  CHECK(r.engine->reduce_only_count() == 2);
  const std::vector<OutCancelMsg> cs = r.transport.cancels();
  REQUIRE(cs.size() == cancels + 1);
  CHECK(side_of(r, cs.back().cl_ord_id) == Side::Buy);

  r.control(ControlCommand::ReduceOnly, InstrumentId::invalid(), VenueId::invalid(), 0);
  CHECK(r.engine->reduce_only(kA));  // the window still holds it
  r.tick(seconds(1800));
  CHECK(r.engine->windows_open() == 0);
  CHECK_FALSE(r.engine->reduce_only(kA));
  CHECK(r.engine->reduce_only_count() == 0);
}
