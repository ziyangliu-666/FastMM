// The control plane the engine implements (docs/how-to/operations/operate-a-running-session.md):
// per-instrument and per-venue pulls, new risk limits, and the engine-owned flatten.
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
  [[nodiscard]] std::size_t count(EventType t) const {
    std::size_t n = 0;
    for (const auto& m : out) n += reinterpret_cast<const EventHeader*>(m.data())->type == t;
    return n;
  }
};

// Quotes both sides of the mid whenever a book arrives, and records what set_quotes answered.
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
static_assert(verify_strategy<Quoter>());

struct Rig {
  using E = Engine<Quoter, SimClock, Transport, InlineFeed>;
  InstrumentTable table = make_table();
  SimClock clock{Timestamp{seconds(1000).ns}};
  Transport transport;
  InlineFeed feed{1 << 20};
  Quoter strategy;
  std::unique_ptr<E> engine;

  explicit Rig(Duration flatten_timeout = seconds(60)) {
    EngineConfig cfg;
    cfg.risk.max_open_orders = 8;
    cfg.quotes.min_requote_interval = Duration{};
    cfg.flatten_interval = milliseconds(100);
    cfg.flatten_timeout = flatten_timeout;
    cfg.flatten_slippage_bps = 10;
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
  // A fill on an order the engine never sent: the position is what a flatten works off.
  void fill(InstrumentId id, Side side, const char* price, const char* qty) {
    fill_order(id, ClientOrderId{0xF00D + id.value}, side, price, qty);
  }
  void fill_order(
      InstrumentId id, ClientOrderId cl, Side side, const char* price, const char* qty) {
    OrderFillMsg m{};
    init_header(m, EventType::OrderFill, id, table.get(id).venue);
    m.cl_ord_id = cl;
    m.side = side;
    m.price = px(price);
    m.qty = qt(qty);
    m.exec_id = FixedString<40>(std::to_string(++execs).c_str());
    push(m);
  }
  std::uint32_t execs = 0;
  void tick(Duration d) {
    clock.advance(d);
    drain();
  }
};

}  // namespace

TEST_CASE("core.engine.control: a pull with an instrument leaves the other instruments quoting") {
  Rig r;
  r.book(kA, "100.00", "100.02");
  r.book(kB, "200.00", "200.02");
  CHECK(r.strategy.accepted == std::vector<bool>{true, true});

  r.control(ControlCommand::PullQuotes, kA);
  CHECK(r.engine->quoting_enabled());  // the session as a whole keeps quoting
  CHECK_FALSE(r.engine->quoting_enabled(kA));
  CHECK(r.engine->quoting_enabled(kB));
  r.book(kA, "100.00", "100.02");
  r.book(kB, "200.00", "200.02");
  CHECK(r.strategy.accepted == std::vector<bool>{true, true, false, true});

  r.control(ControlCommand::ResumeQuotes, kA);
  r.book(kA, "100.00", "100.02");
  CHECK(r.strategy.accepted.back());
}

TEST_CASE("core.engine.control: a pull with a venue covers that venue's instruments") {
  Rig r;
  r.control(ControlCommand::PullQuotes, InstrumentId::invalid(), VenueId{1});
  CHECK(r.engine->quoting_enabled(kA));
  CHECK_FALSE(r.engine->quoting_enabled(kB));
  r.book(kA, "100.00", "100.02");
  r.book(kB, "200.00", "200.02");
  CHECK(r.strategy.accepted == std::vector<bool>{true, false});

  // A resume without a scope takes the whole session back, scoped pulls included.
  r.control(ControlCommand::ResumeQuotes);
  CHECK(r.engine->quoting_enabled(kB));
  r.book(kB, "200.00", "200.02");
  CHECK(r.strategy.accepted.back());
}

TEST_CASE("core.engine.control: SetLimits replaces the risk limits") {
  Rig r;
  CHECK(r.engine->risk().limits().max_order_qty.is_zero());
  ControlLimitsMsg m{};
  init_header(m, EventType::Control);
  m.command = ControlCommand::SetLimits;
  m.limits.max_order_qty = qt("0.001");
  m.limits.max_open_orders = 8;
  r.push(m);
  CHECK(r.engine->risk().limits().max_order_qty == qt("0.001"));
  // The new cap applies to the next quote: 0.010 is over it.
  r.book(kA, "100.00", "100.02");
  CHECK(r.transport.news().empty());
  CHECK(r.engine->stats().risk_rejects > 0);
}

// The status file and fastmm_max_loss carry the limit the engine applies now, not the config's.
TEST_CASE("core.engine.control: the published max_loss follows SetLimits") {
  Rig r;
  ControlLimitsMsg m{};
  init_header(m, EventType::Control);
  m.command = ControlCommand::SetLimits;
  m.limits = r.engine->risk().limits();
  m.limits.max_loss = Notional::from_decimal("250").value();
  r.push(m);
  ControlMsg flush{};
  init_header(flush, EventType::Control);
  flush.command = ControlCommand::FlushStats;
  r.push(flush);
  CHECK(r.engine->live_stats().max_loss_raw == Notional::from_decimal("250").value().raw);
}

TEST_CASE("core.engine.control: flatten sells a long position through the touch, reduce-only") {
  Rig r;
  r.book(kA, "100.00", "100.02");
  r.fill(kA, Side::Buy, "100.00", "0.500");
  const std::size_t before = r.transport.news().size();

  r.control(ControlCommand::Flatten, kA, VenueId::invalid(), 10);
  CHECK(r.engine->flatten_state() == FlattenState::Working);
  CHECK_FALSE(r.engine->quoting_enabled(kA));
  CHECK(r.engine->quoting_enabled(kB));  // a scoped flatten leaves the rest trading
  const std::vector<OutNewOrderMsg> news = r.transport.news();
  REQUIRE(news.size() == before + 1);
  const OutNewOrderMsg& o = news.back();
  CHECK(o.side == Side::Sell);
  CHECK(o.qty == qt("0.500"));
  CHECK(o.reduce_only == 1);
  CHECK(o.tif == TimeInForce::Ioc);
  CHECK(o.price == px("99.90"));  // 10 bps through the bid

  // No second order while the first is unacknowledged.
  r.tick(milliseconds(150));
  CHECK(r.transport.news().size() == before + 1);

  // It fills: the next sweep finds nothing left and the flatten ends.
  r.fill_order(kA, o.cl_ord_id, Side::Sell, "99.90", "0.500");
  r.tick(milliseconds(150));
  CHECK(r.engine->position(kA).flat());
  CHECK(r.engine->flatten_state() == FlattenState::Flat);
  CHECK(r.engine->stats().flatten_orders == 1);
  CHECK(r.engine->stats().flattens == 1);
  // Quoting stays off until an operator resumes it.
  CHECK_FALSE(r.engine->quoting_enabled(kA));
  r.control(ControlCommand::ResumeQuotes, kA);
  CHECK(r.engine->quoting_enabled(kA));
}

TEST_CASE("core.engine.control: flatten buys a short position back and covers every instrument") {
  Rig r;
  r.book(kA, "100.00", "100.02");
  r.book(kB, "200.00", "200.02");
  r.fill(kA, Side::Sell, "100.00", "0.250");
  r.fill(kB, Side::Sell, "200.00", "0.100");
  r.control(ControlCommand::Flatten);
  CHECK_FALSE(r.engine->quoting_enabled());  // a flatten without a scope stops the session
  const std::vector<OutNewOrderMsg> news = r.transport.news();
  REQUIRE(news.size() >= 2);
  const OutNewOrderMsg& a = news[news.size() - 2];
  const OutNewOrderMsg& b = news[news.size() - 1];
  CHECK(a.side == Side::Buy);
  CHECK(a.price == px("100.12"));  // 10 bps through the ask, rounded up to the tick
  CHECK(b.side == Side::Buy);
  CHECK(b.qty == qt("0.100"));
  CHECK(r.engine->stats().flatten_orders == 2);
}

TEST_CASE("core.engine.control: flatten gives up at flatten_timeout and keeps quoting off") {
  Rig r(milliseconds(300));
  r.book(kA, "100.00", "100.02");
  r.fill(kA, Side::Buy, "100.00", "0.500");
  r.control(ControlCommand::Flatten, kA);
  CHECK(r.engine->flatten_state() == FlattenState::Working);
  r.tick(milliseconds(500));
  CHECK(r.engine->flatten_state() == FlattenState::TimedOut);
  CHECK_FALSE(r.engine->position(kA).flat());  // the position is still there: it is an operator's
  CHECK_FALSE(r.engine->quoting_enabled(kA));
  const std::size_t sent = r.transport.news().size();
  r.tick(seconds(2));
  CHECK(r.transport.news().size() == sent);  // it stopped trying
}

TEST_CASE("core.engine.control: a resume stops a running flatten") {
  Rig r;
  r.book(kA, "100.00", "100.02");
  r.fill(kA, Side::Buy, "100.00", "0.500");
  r.control(ControlCommand::Flatten);
  CHECK(r.engine->flatten_state() == FlattenState::Working);
  r.control(ControlCommand::ResumeQuotes);
  CHECK(r.engine->flatten_state() == FlattenState::Stopped);
  CHECK(r.engine->quoting_enabled(kA));
  const std::size_t sent = r.transport.news().size();
  r.tick(seconds(1));
  CHECK(r.transport.news().size() == sent);
}

TEST_CASE("core.engine.control: a flatten order is exempt from max_position") {
  Rig r;
  ControlLimitsMsg m{};
  init_header(m, EventType::Control);
  m.command = ControlCommand::SetLimits;
  m.limits.max_position = qt("0.001");
  m.limits.max_open_orders = 8;
  r.push(m);
  r.book(kA, "100.00", "100.02");
  r.fill(kA, Side::Buy, "100.00", "0.500");  // well over the cap: the flatten must still go out
  r.control(ControlCommand::Flatten, kA);
  REQUIRE_FALSE(r.transport.news().empty());
  CHECK(r.transport.news().back().qty == qt("0.500"));
  CHECK(r.engine->stats().flatten_orders == 1);
}
